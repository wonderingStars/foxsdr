// demod_scope_face.cpp - the DEMOD SCOPE's tube.
//
// WHAT THIS IS TRYING TO LOOK LIKE, and it is not the radar scope. That face
// is a plan-position indicator: a round tube, a rotating sweep, a long-persist
// phosphor. This is the other instrument on the same bench - a rectangular
// service oscilloscope, ruled ten by eight, with a bright short-persist trace
// and an engraved graticule on the inside of the glass. They share the
// product's palette and nothing else, because they are two different machines
// and drawing them the same would say they were one.
//
// THE PALETTE RULE THIS FACE OBEYS. theme.hpp: amber is a NUMBER and nothing
// else; phosphor is what the radio produced. So the beam and the graticule are
// phosphor, every readout on the glass is amber, and the words naming those
// readouts are the panel's ivory. There is no third colour on the tube.
//
// WHY THE TRACE IS AN ENVELOPE AND NOT A POLYLINE THROUGH SAMPLES. See
// scopeReduce in gui/demod_scope.hpp: at the long time bases a single column
// holds hundreds of cycles, and one sample per column draws a slow wandering
// line that is not in the signal. A real tube draws the beam through every
// point, and the visible result is a bright band between the extremes - which
// is what the min/max pair per column reproduces.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/demod_scope_face.hpp"

#include "gui/fm_mpx_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {
namespace {

// --- the tube's own optics ---------------------------------------------------
//
// A service scope's glass is nearly black with a green cast, not the radar
// tube's #0a1c0d: a short-persist P31 screen sits dark between sweeps and the
// trace is what is bright. These are the theme's phosphor taken down to the
// ground and up to the beam rather than a second green invented here.
constexpr ImU32 kGlass = IM_COL32(9, 18, 11, 255);
constexpr ImU32 kGlassEdge = IM_COL32(5, 10, 6, 255);
// The graticule, engraved on the inside of the face: faint for the ordinary
// rules, brighter for the two axes, which is how a hand finds zero volts and
// mid-sweep without counting squares.
constexpr ImU32 kRule = IM_COL32(143, 217, 160, 38);
constexpr ImU32 kRuleAxis = IM_COL32(143, 217, 160, 92);
constexpr ImU32 kRuleTick = IM_COL32(143, 217, 160, 70);
// The beam, and the bloom around it. A CRT's spot is not one pixel wide; the
// halo is what keeps a fast trace visible when the envelope in a column is a
// single line.
constexpr ImU32 kBeam = IM_COL32(180, 255, 195, 235);
constexpr ImU32 kBeamGlow = IM_COL32(143, 217, 160, 64);
// The SECOND beam, for the Q trace beside I. Dimmer and cooler, so which is
// which can be read from the picture and not only from the legend beside it.
constexpr ImU32 kBeamQ = IM_COL32(120, 200, 235, 225);
constexpr ImU32 kBeamQGlow = IM_COL32(120, 200, 235, 56);
// One dark row every three pixels: the same scan-line treatment the radar tube
// wears, so the two faces read as glass from the same era.
constexpr ImU32 kScanLine = IM_COL32(0, 0, 0, 30);

// A word on the glass, in the panel's own faces. Kept as one helper because
// every caption on this face is set the same way and a second hand-rolled
// PushFont here is exactly where drift starts.
void glassText(ImDrawList* dl, ImFont* font, float px, const ImVec2& at, ImU32 col,
               const char* text) {
    if (dl == nullptr || font == nullptr || text == nullptr || text[0] == '\0') { return; }
    dl->AddText(font, px, at, col, text);
}

// Right-aligned, because the readouts along the foot of the glass are a row of
// values whose right edges have to line up or the row reads as a jumble.
float textWidth(ImFont* font, float px, const char* text) {
    if (font == nullptr || text == nullptr) { return 0.0f; }
    return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
}

// --- the graticule -----------------------------------------------------------
void addGraticule(ImDrawList* dl, const ImVec2& a, const ImVec2& b) {
    for (int i = 0; i <= kScopeDivX; ++i) {
        const float x = scopeGridLine(i, kScopeDivX, a.x, b.x);
        const bool axis = scopeGridIsCentre(i, kScopeDivX);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), axis ? kRuleAxis : kRule, 1.0f);
    }
    for (int i = 0; i <= kScopeDivY; ++i) {
        const float y = scopeGridLine(i, kScopeDivY, a.y, b.y);
        const bool axis = scopeGridIsCentre(i, kScopeDivY);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), axis ? kRuleAxis : kRule, 1.0f);
    }

    // THE MINOR TICKS ON THE TWO AXES, five to a division. This is the detail
    // that separates a ruled rectangle from a graticule: a reading is
    // interpolated between them, and a scope with no subdivisions can only be
    // read to the nearest whole square.
    const float cy = scopeGridLine(kScopeDivY / 2, kScopeDivY, a.y, b.y);
    const float cx = scopeGridLine(kScopeDivX / 2, kScopeDivX, a.x, b.x);
    const float divW = (b.x - a.x) / static_cast<float>(kScopeDivX);
    const float divH = (b.y - a.y) / static_cast<float>(kScopeDivY);
    for (int d = 0; d < kScopeDivX; ++d) {
        for (int s = 1; s < 5; ++s) {
            const float x = a.x + divW * (static_cast<float>(d) + static_cast<float>(s) / 5.0f);
            dl->AddLine(ImVec2(x, cy - 3.0f), ImVec2(x, cy + 3.0f), kRuleTick, 1.0f);
        }
    }
    for (int d = 0; d < kScopeDivY; ++d) {
        for (int s = 1; s < 5; ++s) {
            const float y = a.y + divH * (static_cast<float>(d) + static_cast<float>(s) / 5.0f);
            dl->AddLine(ImVec2(cx - 3.0f, y), ImVec2(cx + 3.0f, y), kRuleTick, 1.0f);
        }
    }
}

void addScanLines(ImDrawList* dl, const ImVec2& a, const ImVec2& b) {
    for (float y = a.y + 1.5f; y < b.y; y += 3.0f) {
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), kScanLine, 1.0f);
    }
}

// --- one trace ---------------------------------------------------------------
//
// Draws the envelope already reduced into lo/hi as a filled band with the beam
// on its edges. A column whose extremes coincide (a slow waveform, one sample
// per column) still gets a mark, because a zero-height band would draw nothing
// at all and a scope showing a straight line as nothing is a scope that has
// broken exactly when the signal is simplest.
void addTrace(ImDrawList* dl, const float* lo, const float* hi, std::size_t cols,
              float x0, float unitsPerDiv, float yCentre, float divPx, ImU32 beam,
              ImU32 glow) {
    if (lo == nullptr || hi == nullptr || cols == 0) { return; }
    float prevY = yCentre;
    for (std::size_t c = 0; c < cols; ++c) {
        const float x = x0 + static_cast<float>(c);
        const float yHi = scopeTraceY(hi[c], unitsPerDiv, yCentre, divPx);
        const float yLo = scopeTraceY(lo[c], unitsPerDiv, yCentre, divPx);
        // The bloom first, so the beam is drawn over its own halo.
        dl->AddLine(ImVec2(x, yHi - 1.5f), ImVec2(x, yLo + 1.5f), glow, 2.5f);
        dl->AddLine(ImVec2(x, yHi), ImVec2(x, yLo + 1.0f), beam, 1.0f);
        // JOIN THE COLUMNS. Without this a steep edge is drawn as two
        // disconnected vertical marks with a gap between them, which is the
        // one artefact that makes a digital scope look digital.
        if (c > 0) {
            const float join0 = std::min(prevY, yLo);
            const float join1 = std::max(prevY, yHi);
            if (join1 - join0 > 1.0f) {
                dl->AddLine(ImVec2(x, join0), ImVec2(x, join1), beam, 1.0f);
            }
        }
        prevY = 0.5f * (yHi + yLo);
    }
}

// --- the readout row ---------------------------------------------------------
//
// Along the foot of the glass, inside it: what is being looked at, how fast
// the sweep is, and what one division is worth. The FIGURES are amber on the
// glass, which is the palette's rule for a reading; the words naming them are
// the panel's ivory.
void addReadouts(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
                 const DemodScopeFeed& feed, const DemodScopeState& state,
                 ScopeSignal signal) {
    ImFont* legend = fonts::legend();
    ImFont* reading = fonts::reading();
    const float px = fonts::kTinySize;
    const float y = b.y - px - 5.0f;

    glassText(dl, legend, px, ImVec2(a.x + 7.0f, y), theme::kIvory,
              scopeSignalCaption(signal));

    const bool spectral = (signal == ScopeSignal::Spectrum || signal == ScopeSignal::Mpx);

    char buf[32];
    if (spectral) {
        // A SPECTRUM HAS NO TIME BASE, and printing one would be a number that
        // describes nothing on the screen. What the axis is ruled in is the
        // span, so that is what the row says instead. The span comes from the
        // feed rather than being recomputed here, so the label and the bins
        // the page actually cut cannot disagree - which matters more for the
        // multiplex, where the labels are placed against that same number.
        const double span = (feed.spectrumSpanHz > 0.0)
                                ? feed.spectrumSpanHz
                                : scopeSpectrumSpanHz(feed.audioRateHz);
        std::snprintf(buf, sizeof(buf), "0 - %.1f kHz", span / 1000.0);
    } else {
        const double rate = scopeSignalIsBaseband(signal) ? feed.iqRateHz : feed.audioRateHz;
        if (rate > 0.0) {
            formatScopeTimebase(buf, sizeof(buf), scopeTimebaseMs(state.timebase));
        } else {
            std::snprintf(buf, sizeof(buf), "--");
        }
    }
    const float midW = textWidth(reading, px, buf);
    glassText(dl, reading, px, ImVec2((a.x + b.x) * 0.5f - midW * 0.5f, y), theme::kAmber,
              buf);

    char gain[32];
    if (spectral) {
        std::snprintf(gain, sizeof(gain), "10 dB/DIV");
    } else {
        formatScopeGain(gain, sizeof(gain), scopeGainPerDiv(state.gainIndex));
    }
    const float rightW = textWidth(reading, px, gain);
    glassText(dl, reading, px, ImVec2(b.x - 7.0f - rightW, y), theme::kAmber, gain);

    // WHOSE SOUND THIS IS, when it is not the demodulator's. The tap is below
    // the CASCADE_CAP_AUDIO_OUT handover, so the trace really is the plugin's
    // audio; a face that did not say so would be crediting a decoded broadcast
    // to the discriminator.
    if (feed.audioFrom != nullptr && feed.audioFrom[0] != '\0' &&
        signal != ScopeSignal::Baseband && signal != ScopeSignal::Vector) {
        char via[96];
        std::snprintf(via, sizeof(via), "AUDIO FROM %s", feed.audioFrom);
        glassText(dl, legend, px, ImVec2(a.x + 7.0f, a.y + 5.0f), theme::kAmber, via);
    }
}

// The one sentence a dead tube is allowed to say, centred on the glass. Not a
// flat trace at zero: that claims a measurement that was not made.
void addNoSignal(ImDrawList* dl, const ImVec2& a, const ImVec2& b, const char* why) {
    ImFont* f = fonts::legend();
    const float px = fonts::kLegendSize;
    const float w = textWidth(f, px, why);
    glassText(dl, f, px,
              ImVec2((a.x + b.x) * 0.5f - w * 0.5f, (a.y + b.y) * 0.5f - px * 0.5f),
              theme::kPhosphorDim, why);
}

}  // namespace

float demodScopePeak(const DemodScopeFeed& feed, ScopeSignal signal) {
    if (scopeSignalIsBaseband(signal)) {
        // BOTH ARMS, ONE PEAK. I and Q share an attenuator - they have to, or
        // a circle is drawn as an ellipse and the vector display's whole claim
        // about the signal's shape is wrong - so the ranging is fed whichever
        // of them is larger.
        const float pi = scopePeak(feed.iqI, feed.iqCount);
        const float pq = scopePeak(feed.iqQ, feed.iqCount);
        return (pi > pq) ? pi : pq;
    }
    return scopePeak(feed.audio, feed.audioCount);
}

// WHAT EACH PART OF THE MULTIPLEX IS, written on the glass.
//
// The request this exists for (owner, 2026-09-21): a view of the broadcast FM
// spectrum "where it shows what each frequency group is used for". The table
// and the arithmetic are in gui/fm_mpx_plan.hpp and are tested without a draw
// list; this is the ink.
//
// HOW IT READS. Each region gets a bracket along the top of the glass - a rule
// with a tick turned down at each end, which says "this extent" without
// filling the area the trace lives in - and its name under the bracket. A tone
// (the pilot, an SCA) gets one vertical rule the full height instead, because
// a bracket a pixel wide is not a bracket. Anything a station may simply not
// transmit is drawn fainter than the sum channel and the pilot: an empty
// 57 kHz slot is a fact about the station, and a label in the same ink as the
// pilot's would read as a fault in the receiver.
//
// CROWDING IS DECIDED BY MEASUREMENT, not by a width threshold picked by eye:
// a label is drawn if the text fits in the room its own band owns, and skipped
// if it does not. On a narrow tube that leaves "Mono L+R" and the pilot rule,
// which is the right answer rather than a compromise.
void drawMpxLabels(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
                   const DemodScopeFeed& feed) {
    const double span = (feed.spectrumSpanHz > 0.0)
                            ? feed.spectrumSpanHz
                            : static_cast<double>(feed.spectrumBins) * feed.spectrumBinHz;
    if (!(span > 0.0)) { return; }
    const float w = b.x - a.x;
    if (!(w > 0.0f)) { return; }

    ImFont* reading = fonts::reading();
    const float px = fonts::kTinySize;
    // The bracket sits under the top rule; band names hang under the bracket
    // and tone names one line lower, so "Pilot" cannot land on top of the sum
    // channel's name where the two nearly touch.
    const float bracketY = a.y + 10.0f;
    const float bandTextY = bracketY + 3.0f;
    const float toneTextY = bandTextY + px + 2.0f;

    // THE TUBE'S OWN INK, not a new palette: the rules are drawn in the same
    // phosphor as the graticule (kRuleAxis for what every station sends,
    // kRule for what it may not) so they read as part of the glass rather
    // than as an overlay pasted on top of it.
    const ImU32 inkStrong = kRuleAxis;
    const ImU32 inkFaint = kRule;
    const ImU32 textStrong = theme::withAlpha(theme::kIvory, 0.80f);
    const ImU32 textFaint = theme::withAlpha(theme::kIvory, 0.46f);

    for (std::size_t i = 0; i < kMpxBandCount; ++i) {
        const MpxBand& band = kMpxBands[i];
        if (!mpxBandInSpan(band, span)) { continue; }
        const ImU32 ink = band.optional ? inkFaint : inkStrong;
        const ImU32 text = band.optional ? textFaint : textStrong;

        if (band.loHz == band.hiHz) {
            // A TONE: one rule, floor to ceiling, and its name beside the
            // rule's foot rather than centred on it - centred, the text would
            // sit astride the very line it is naming.
            const float x = a.x + w * static_cast<float>(mpxFraction(band.loHz, span));
            dl->AddLine(ImVec2(x, a.y + 2.0f), ImVec2(x, b.y - 2.0f), ink, 1.0f);
            const float tw = textWidth(reading, px, band.label);
            if (tw + 6.0f <= mpxLabelRoomPx(band, span, w)) {
                float tx = x + 3.0f;
                if (tx + tw > b.x - 2.0f) { tx = x - 3.0f - tw; }
                glassText(dl, reading, px, ImVec2(tx, toneTextY), text, band.label);
            }
            continue;
        }

        // AN EXTENT: the bracket, then the name centred under it.
        const double hi = (band.hiHz < span) ? band.hiHz : span;
        const float x0 = a.x + w * static_cast<float>(mpxFraction(band.loHz, span));
        const float x1 = a.x + w * static_cast<float>(mpxFraction(hi, span));
        dl->AddLine(ImVec2(x0, bracketY), ImVec2(x1, bracketY), ink, 1.0f);
        dl->AddLine(ImVec2(x0, bracketY), ImVec2(x0, bracketY + 4.0f), ink, 1.0f);
        dl->AddLine(ImVec2(x1, bracketY), ImVec2(x1, bracketY + 4.0f), ink, 1.0f);

        const float tw = textWidth(reading, px, band.label);
        if (tw + 4.0f <= (x1 - x0)) {
            glassText(dl, reading, px, ImVec2((x0 + x1) * 0.5f - tw * 0.5f, bandTextY), text,
                      band.label);
        }
    }
}

void drawDemodScopeFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                        const DemodScopeFeed& feed, const DemodScopeState& state,
                        float* lo, float* hi, std::size_t scratchCap) {
    if (dl == nullptr) { return; }
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    // Under this there is no room for a graticule, let alone a reading on it.
    // Drawing a two-division scope would be worse than drawing none: a ruler
    // with the wrong number of marks on it is a measuring instrument that
    // lies.
    if (w < 120.0f || h < 100.0f) { return; }

    const ScopeSignal signal = scopeSignalFromIndex(state.signal);

    // The bay the tube is sunk into, then the bezel, then the glass. Same
    // vocabulary as every other recessed thing on this bench, from
    // scope_face.hpp, so the scope reads as part of the same machine.
    addScopeBay(dl, tl, br, false);
    const ImVec2 bezelTL(tl.x + 6.0f, tl.y + 6.0f);
    const ImVec2 bezelBR(br.x - 6.0f, br.y - 6.0f);
    dl->AddRectFilled(bezelTL, bezelBR, theme::kEnamelDark, 3.0f);
    addBenchBevel(dl, bezelTL, bezelBR, 3.0f, false);
    const ImVec2 a(bezelTL.x + 5.0f, bezelTL.y + 5.0f);
    const ImVec2 b(bezelBR.x - 5.0f, bezelBR.y - 5.0f);
    dl->AddRectFilled(a, b, kGlass, 2.0f);
    dl->AddRect(a, b, kGlassEdge, 2.0f, 0, 2.0f);

    // THE GRATICULE IS RULED ON THE WHOLE GLASS and the trace lives inside it,
    // which is why the drawing area is the graticule and not some inset of it:
    // "four divisions" has to mean four of the squares a user can see.
    dl->PushClipRect(a, b, true);
    addGraticule(dl, a, b);

    const float divPx = (b.y - a.y) / static_cast<float>(kScopeDivY);
    const float yCentre = scopeGridLine(kScopeDivY / 2, kScopeDivY, a.y, b.y);
    const std::size_t cols =
        std::min(scratchCap, static_cast<std::size_t>(std::max(1.0f, b.x - a.x)));
    const float unitsPerDiv = scopeGainPerDiv(state.gainIndex);

    if (!feed.live) {
        addNoSignal(dl, a, b, "NO SAMPLES - RECEIVER STOPPED");
    } else {
        switch (signal) {
            case ScopeSignal::Audio: {
                if (feed.audio != nullptr && feed.audioCount > 0 && lo != nullptr &&
                    hi != nullptr && scopeReduce(feed.audio, feed.audioCount, cols, lo, hi)) {
                    addTrace(dl, lo, hi, cols, a.x, unitsPerDiv, yCentre, divPx, kBeam,
                             kBeamGlow);
                } else {
                    addNoSignal(dl, a, b, "NO AUDIO IN THE TAP");
                }
                break;
            }
            case ScopeSignal::Spectrum:
            case ScopeSignal::Mpx: {
                const char* empty = (signal == ScopeSignal::Mpx)
                                        ? "NO MULTIPLEX IN THE TAP"
                                        : "NO AUDIO IN THE TAP";
                if (feed.spectrumDb == nullptr || feed.spectrumBins < 2 ||
                    !(feed.spectrumBinHz > 0.0)) {
                    addNoSignal(dl, a, b, empty);
                    break;
                }
                // THE LABELS GO UNDER THE BEAM, always: they are a ruler laid
                // on the glass, and a ruler that hid the measurement would be
                // the wrong way round.
                if (signal == ScopeSignal::Mpx) { drawMpxLabels(dl, a, b, feed); }
                const std::size_t n = feed.spectrumBins;
                // A LINE PER BIN, not a bar chart: the spectrum shares its
                // tube with the traces above, and a filled histogram would
                // read as a different instrument on the same glass.
                float prevX = a.x;
                float prevY = scopeSpectrumY(feed.spectrumDb[0], a.y, b.y);
                for (std::size_t k = 1; k < n; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(n - 1);
                    const float x = a.x + (b.x - a.x) * t;
                    const float y = scopeSpectrumY(feed.spectrumDb[k], a.y, b.y);
                    dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), kBeamGlow, 3.0f);
                    dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, y), kBeam, 1.0f);
                    prevX = x;
                    prevY = y;
                }
                break;
            }
            case ScopeSignal::Baseband: {
                if (feed.iqI == nullptr || feed.iqQ == nullptr || feed.iqCount == 0 ||
                    lo == nullptr || hi == nullptr) {
                    addNoSignal(dl, a, b, "NO BASEBAND IN THE TAP");
                    break;
                }
                // TWO TRACES, TWO HALVES OF THE TUBE. I above the axis and Q
                // below it: overlaid on one axis they would cross constantly
                // and neither could be followed, which is what every
                // dual-trace scope's position controls exist to avoid.
                const float qCentre = yCentre + divPx * 2.0f;
                const float iCentre = yCentre - divPx * 2.0f;
                if (scopeReduce(feed.iqI, feed.iqCount, cols, lo, hi)) {
                    addTrace(dl, lo, hi, cols, a.x, unitsPerDiv, iCentre, divPx, kBeam,
                             kBeamGlow);
                }
                if (scopeReduce(feed.iqQ, feed.iqCount, cols, lo, hi)) {
                    addTrace(dl, lo, hi, cols, a.x, unitsPerDiv, qCentre, divPx, kBeamQ,
                             kBeamQGlow);
                }
                // EACH LETTER ONE DIVISION ABOVE ITS OWN TRACE, which is
                // inside the glass. Two divisions put the I above the top rule
                // and out of the clip rectangle entirely: the picture came
                // back with a Q and no I, and a two-trace display that names
                // one of its traces is worse than one that names neither.
                ImFont* lf = fonts::legend();
                glassText(dl, lf, fonts::kTinySize,
                          ImVec2(a.x + 5.0f, iCentre - divPx - fonts::kTinySize),
                          theme::kPhosphor, "I");
                glassText(dl, lf, fonts::kTinySize,
                          ImVec2(a.x + 5.0f, qCentre - divPx - fonts::kTinySize),
                          theme::kPhosphorDim, "Q");
                break;
            }
            case ScopeSignal::Vector: {
                if (feed.iqI == nullptr || feed.iqQ == nullptr || feed.iqCount == 0) {
                    addNoSignal(dl, a, b, "NO BASEBAND IN THE TAP");
                    break;
                }
                const float cx = scopeGridLine(kScopeDivX / 2, kScopeDivX, a.x, b.x);
                // THE BEAM IS DRAWN AS A PATH, not as points: the eye reads a
                // Lissajous from the line the spot traces, and a scatter of
                // dots is a constellation diagram, which is a different
                // instrument making a different claim.
                //
                // AND IT IS THINNED to something a frame can carry. A half
                // second of channel I/Q is a hundred thousand points; four
                // thousand is more than enough to close the figure, and the
                // ones dropped are indistinguishable at this radius.
                constexpr std::size_t kMaxPoints = 4096;
                const std::size_t step =
                    (feed.iqCount + kMaxPoints - 1) / kMaxPoints;
                float px0 = 0.0f;
                float py0 = 0.0f;
                bool have = false;
                for (std::size_t k = 0; k < feed.iqCount; k += (step > 0 ? step : 1)) {
                    float x = 0.0f;
                    float y = 0.0f;
                    scopeVectorPoint(feed.iqI[k], feed.iqQ[k], unitsPerDiv, cx, yCentre,
                                     divPx, x, y);
                    if (have) {
                        dl->AddLine(ImVec2(px0, py0), ImVec2(x, y), kBeamGlow, 2.5f);
                        dl->AddLine(ImVec2(px0, py0), ImVec2(x, y), kBeam, 1.0f);
                    }
                    px0 = x;
                    py0 = y;
                    have = true;
                }
                break;
            }
        }
    }

    addScanLines(dl, a, b);
    addReadouts(dl, a, b, feed, state, signal);
    dl->PopClipRect();
}

}  // namespace cascade::gui
