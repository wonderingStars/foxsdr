// instrument_face.cpp - dispatch by kind, the generic readout, and the chip.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/instrument_face.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/fonts.hpp"
#include "gui/instrument_beacon_math.hpp"
#include "gui/instrument_meter_math.hpp"
#include "gui/instrument_teleprinter_math.hpp"
#include "gui/instrument_weather_console_math.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::core::HostInstrument;

float drawInstrumentFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                         const HostInstrument& in, const InstrumentCue& cue) {
    switch (in.kind) {
        case CASCADE_INSTRUMENT_PAGER: return drawPagerFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_TELEPRINTER: return drawTeleprinterFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_TONE_ALERT: return drawToneAlertFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_NAV_BEARING: return drawNavBearingFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_FAX: return drawFaxFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_BEACON: return drawBeaconFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_METER: return drawMeterFace(dl, tl, br, in, cue);
        case CASCADE_INSTRUMENT_WEATHER_CONSOLE:
            return drawWeatherConsoleFace(dl, tl, br, in, cue);
        default: return drawGenericFace(dl, tl, br, in, cue);
    }
}

namespace {

// A caption cut into the plate, in the legend face at the small engraving
// size, muted ink. Returns the width used.
float engrave(ImDrawList* dl, const ImVec2& at, const char* s) {
    ImFont* f = fonts::legend();
    dl->AddText(f, fonts::kTinySize, ImVec2(at.x, at.y + 1.0f), theme::kVoid, s);
    dl->AddText(f, fonts::kTinySize, at, theme::kInkMuted, s);
    return f->CalcTextSizeA(fonts::kTinySize, FLT_MAX, 0.0f, s).x;
}

// A live word on glass, in the ui face, phosphor.
void onGlass(ImDrawList* dl, const ImVec2& at, const char* s, float maxW) {
    ImFont* f = fonts::ui();
    dl->AddText(f, fonts::kUiSize, at, theme::kPhosphor, s, nullptr, maxW);
}

// A live figure on glass, in the reading face.
void figure(ImDrawList* dl, const ImVec2& at, const char* s) {
    dl->AddText(fonts::reading(), fonts::kReadingSize, at, theme::kPhosphor, s);
}

}  // namespace

float drawGenericFace(ImDrawList* dl, const ImVec2& tl, const ImVec2& br,
                      const HostInstrument& in, const InstrumentCue& cue) {
    if (dl == nullptr) { return 0.0f; }
    const float w = br.x - tl.x;
    if (w < 80.0f || br.y - tl.y < 60.0f) { return 0.0f; }

    // The plate, titled by the plugin's own window name.
    float y = addBenchPlate(dl, tl, br, in.title.c_str());

    // The lamps, on the plate's right shoulder: what is ringing, what is
    // locked, what has not been looked at. Each is drawn whether lit or not,
    // so a cold panel still says which lamps it has.
    const float lampR = 6.0f;
    const float lampPitch = 64.0f;
    float lx = br.x - 12.0f - lampR;
    const float ly = y + 12.0f;
    const bool alert = in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u;
    const bool lock = in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u;
    const bool lowBatt = in.have && (in.state.flags & CASCADE_INSTRUMENT_FLAG_LOW_BATT) != 0u;
    // A ringing lamp blinks at 2 Hz, which is what tells it apart from one
    // that is simply on.
    const bool blinkOn = std::fmod(cue.nowSec, 0.5) < 0.25;
    drawBenchLamp(dl, ImVec2(lx, ly), lampR, theme::kAlarmHot, alert && blinkOn, "ALERT");
    lx -= lampPitch;
    drawBenchLamp(dl, ImVec2(lx, ly), lampR, theme::kGold, cue.unread, "NEW");
    lx -= lampPitch;
    drawBenchLamp(dl, ImVec2(lx, ly), lampR, theme::kPhosphor, lock, "LOCK");
    lx -= lampPitch;
    drawBenchLamp(dl, ImVec2(lx, ly), lampR, theme::kAmber, lowBatt, "BATT");
    y = ly + lampR + fonts::kTinySize + 10.0f;

    // The glass: every filled slot, labelled by its number, texts down the
    // left and figures down the right. This is the face a kind gets before
    // it has one of its own, and the face any newer kind gets here, so it
    // must show EVERYTHING the plugin sent and invent nothing.
    const ImVec2 gtl(tl.x + 10.0f, y);
    const ImVec2 gbr(br.x - 10.0f, br.y - 10.0f);
    if (gbr.y - gtl.y < 30.0f) { return y - tl.y; }
    drawFreqDrumWell(dl, gtl, gbr);
    dl->PushClipRect(gtl, gbr, true);
    const float lineH = fonts::kUiSize + 4.0f;
    float ty = gtl.y + 8.0f;
    if (!in.have) {
        engrave(dl, ImVec2(gtl.x + 10.0f, ty), "NO READING YET");
    } else {
        const float split = gtl.x + (gbr.x - gtl.x) * 0.62f;
        float vy = ty;
        for (int i = 0; i < CASCADE_INSTRUMENT_TEXTS; ++i) {
            if (in.state.text[i][0] == '\0') { continue; }
            char cap[8];
            std::snprintf(cap, sizeof cap, "T%d", i);
            engrave(dl, ImVec2(gtl.x + 8.0f, ty + 3.0f), cap);
            onGlass(dl, ImVec2(gtl.x + 36.0f, ty), in.state.text[i], split - gtl.x - 44.0f);
            ty += lineH;
        }
        for (int i = 0; i < CASCADE_INSTRUMENT_VALUES; ++i) {
            if (in.state.values[i] == 0.0) { continue; }
            char cap[8];
            std::snprintf(cap, sizeof cap, "V%d", i);
            engrave(dl, ImVec2(split + 4.0f, vy + 3.0f), cap);
            char num[32];
            std::snprintf(num, sizeof num, "%.6g", in.state.values[i]);
            figure(dl, ImVec2(split + 32.0f, vy), num);
            vy += lineH;
        }
        char seq[32];
        std::snprintf(seq, sizeof seq, "EVENT %u", static_cast<unsigned>(in.state.seq));
        engrave(dl, ImVec2(gtl.x + 8.0f, gbr.y - fonts::kTinySize - 6.0f), seq);
    }
    dl->PopClipRect();
    return gbr.y + 10.0f - tl.y;
}

void instrumentChip(const HostInstrument& in, bool unread, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    if (unread) {
        std::snprintf(out, cap, "NEW");
        return;
    }
    if (!in.have) {
        std::snprintf(out, cap, "WAIT");
        return;
    }
    const CascadeInstrumentState& s = in.state;
    switch (in.kind) {
        case CASCADE_INSTRUMENT_PAGER:
            std::snprintf(out, cap, "%d MSG", static_cast<int>(std::lround(s.values[0])));
            return;
        case CASCADE_INSTRUMENT_TELEPRINTER:
            // A PRINTER'S IDLE WORD IS WHAT IT LAST PRINTED, not how much it
            // has printed: "G-EZBX" on the rail says an aircraft was heard
            // and which one, where "12 MSG" says only that the counter has
            // moved. The count remains the fallback for a feed that fills no
            // station slot. teleprinter::chipWord is where both live, so the
            // rail's word and the face's arithmetic are pinned by one test.
            teleprinter::chipWord(s.text[0], s.values[0], out, cap);
            return;
        case CASCADE_INSTRUMENT_TONE_ALERT:
            if ((s.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u) {
                std::snprintf(out, cap, "ALERT");
            } else {
                std::snprintf(out, cap, "%d LOG", static_cast<int>(in.rows.size()));
            }
            return;
        case CASCADE_INSTRUMENT_BEACON:
            // ALERT while the alert is held, and otherwise HOW LONG AGO the
            // last burst was: with a burst period near fifty seconds that one
            // figure is what says whether the beacon is still transmitting,
            // which the size of the log cannot. instrument_beacon_math.hpp
            // owns the wording so the rail and the face cannot disagree.
            beacon::chipWord((s.flags & CASCADE_INSTRUMENT_FLAG_ALERT) != 0u, s.values[1],
                             static_cast<int>(in.rows.size()), out, cap);
            return;
        case CASCADE_INSTRUMENT_NAV_BEARING:
            if ((s.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u) {
                std::snprintf(out, cap, "%03d", static_cast<int>(std::lround(s.values[0])) % 360);
            } else {
                std::snprintf(out, cap, "NAV");
            }
            return;
        case CASCADE_INSTRUMENT_FAX:
            // THE PHASE WORD, OR THE MACHINE'S NAME - never "IDLE" invented
            // out of an empty slot. A recorder reporting IDLE and one that has
            // sent no phase at all are different claims, and the second is one
            // this host was never told: see rule 2 in instrument_face.hpp.
            if (s.text[0][0] != '\0') {
                std::snprintf(out, cap, "%s", s.text[0]);
            } else {
                std::snprintf(out, cap, "FAX");
            }
            return;
        case CASCADE_INSTRUMENT_METER:
            // The roster count, because a neighbourhood has dozens of meters
            // and how many have been heard is the thing worth knowing without
            // opening the window - but never "0 MTR", which would be a zero
            // standing in for "we have not counted". See instrument_meter_math.
            meter::chipWord(in.have, in.rows.size(), out, cap);
            return;
        case CASCADE_INSTRUMENT_WEATHER_CONSOLE:
            // Through the face's own arithmetic, which is where the channel
            // mask is read safely: values[6] is a double carrying three bits
            // and std::lround of a NaN or a 1e300 in that slot is undefined
            // behaviour, not a big number.
            wxface::chipWord(s.values[6], s.values, out, cap);
            return;
        default:
            std::snprintf(out, cap, "RX");
            return;
    }
}

}  // namespace cascade::gui
