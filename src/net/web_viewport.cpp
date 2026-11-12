// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_viewport.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::net {
namespace {

bool finite(double v) { return std::isfinite(v); }

double minCount(std::int64_t total) { return std::min(kMinVisibleBins, static_cast<double>(total)); }

}  // namespace

WaterfallView fullView(std::int64_t totalBins) {
    WaterfallView v;
    v.first = 0.0;
    v.count = totalBins > 0 ? static_cast<double>(totalBins) : 0.0;
    return v;
}

WaterfallView clampView(WaterfallView v, std::int64_t totalBins) {
    if (totalBins <= 0) { return fullView(0); }
    if (!finite(v.first) || !finite(v.count)) { return fullView(totalBins); }
    const double total = static_cast<double>(totalBins);
    v.count = std::min(std::max(v.count, minCount(totalBins)), total);
    v.first = std::min(std::max(v.first, 0.0), total - v.count);
    return v;
}

WaterfallView zoomAt(WaterfallView v, std::int64_t totalBins, double anchor, double factor) {
    v = clampView(v, totalBins);
    if (totalBins <= 0 || !finite(factor) || factor <= 0.0) { return v; }
    if (!finite(anchor)) { anchor = 0.5; }
    anchor = std::min(std::max(anchor, 0.0), 1.0);
    const double binUnder = v.first + anchor * v.count;
    WaterfallView n;
    n.count = v.count / factor;
    n.first = binUnder - anchor * n.count;
    return clampView(n, totalBins);
}

double wheelFactor(double deltaY) {
    if (!finite(deltaY) || deltaY == 0.0) { return 1.0; }
    // Up (negative) zooms in: a factor above 1. The exponent is bounded first so a huge delta saturates at the step cap rather than overflowing.
    const double e = std::min(std::max(-deltaY / 100.0, -50.0), 50.0);
    double f = std::pow(kWheelZoomPerNotch, e);
    if (!finite(f) || f <= 0.0) { return 1.0; }
    f = std::min(std::max(f, 1.0 / kMaxZoomStep), kMaxZoomStep);
    return f;
}

WaterfallView panByPixels(WaterfallView v, std::int64_t totalBins, double dxPixels, double widthPixels) {
    v = clampView(v, totalBins);
    if (totalBins <= 0 || !finite(dxPixels) || !finite(widthPixels) || widthPixels <= 0.0) { return v; }
    WaterfallView n = v;
    n.first = v.first - dxPixels / widthPixels * v.count;  // drag right: the picture follows, lower bins appear
    return clampView(n, totalBins);
}

WaterfallView panByBins(WaterfallView v, std::int64_t totalBins, double deltaBins) {
    v = clampView(v, totalBins);
    if (totalBins <= 0 || !finite(deltaBins)) { return v; }
    WaterfallView n = v;
    n.first = v.first + deltaBins;
    return clampView(n, totalBins);
}

double pointerToHz(WaterfallView v, const SpectrumSpan& span, double x, double widthPixels) {
    const double low = span.centreHz - span.spanHz / 2.0;
    if (span.totalBins <= 0 || !finite(span.spanHz) || !finite(span.centreHz) || !finite(widthPixels) || widthPixels <= 0.0) { return low; }
    v = clampView(v, span.totalBins);
    if (!finite(x)) { x = 0.0; }
    x = std::min(std::max(x, 0.0), widthPixels);
    const double bin = v.first + x / widthPixels * v.count;
    return low + bin / static_cast<double>(span.totalBins) * span.spanHz;
}

double hzToPointer(WaterfallView v, const SpectrumSpan& span, double hz, double widthPixels) {
    if (span.totalBins <= 0 || !finite(span.spanHz) || span.spanHz <= 0.0 || !finite(hz) || !finite(widthPixels) || widthPixels <= 0.0) {
        return 0.0;
    }
    v = clampView(v, span.totalBins);
    const double low = span.centreHz - span.spanHz / 2.0;
    const double bin = (hz - low) / span.spanHz * static_cast<double>(span.totalBins);
    return (bin - v.first) / v.count * widthPixels;
}

WaterfallView viewForRange(const SpectrumSpan& span, double loHz, double hiHz) {
    if (span.totalBins <= 0) { return fullView(0); }
    if (!finite(loHz) || !finite(hiHz) || !finite(span.spanHz) || span.spanHz <= 0.0 || !finite(span.centreHz)) {
        return fullView(span.totalBins);
    }
    if (loHz > hiHz) { std::swap(loHz, hiHz); }
    const double low = span.centreHz - span.spanHz / 2.0;
    const double total = static_cast<double>(span.totalBins);
    WaterfallView v;
    v.first = (loHz - low) / span.spanHz * total;
    v.count = (hiHz - loHz) / span.spanHz * total;
    return clampView(v, span.totalBins);
}

BinRange visibleBins(WaterfallView v, std::int64_t totalBins) {
    BinRange r;
    if (totalBins <= 0) { return r; }
    v = clampView(v, totalBins);
    r.firstBin = static_cast<std::int64_t>(std::floor(v.first));
    r.endBin = static_cast<std::int64_t>(std::ceil(v.first + v.count));
    r.firstBin = std::min(std::max<std::int64_t>(r.firstBin, 0), totalBins);
    r.endBin = std::min(std::max(r.endBin, r.firstBin), totalBins);
    return r;
}

}  // namespace cascade::net
