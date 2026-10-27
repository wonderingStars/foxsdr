// app_window_deck.cpp - THE TOP BAR'S KNOB ROW: the GAIN and SQUELCH dials with their AUTO keys, and the VOLUME dial beside them (stub campaign B12, RAIL-B; master 0.99.74).
// AppWindow members, kept out of app_window.cpp because they are one subject (and because that file is at the object-file limit).
//
// THE REQUEST (the tester, 2026-10-12): "Add a gain knob, just like the Volume knob at the top of the screen with a switch for Auto Gain" and "Add a Squelch knob, like the
// gain knob above but with a button for AUTO ... I would put these knobs in the order of gain, squelch and volume just as that is the order the signal goes through the
// system." The tester "find[s] myself adjusting the gain for certain things often ... without having to go to the Signal tab/Source menu. This area is also empty space",
// and the same was said of the squelch, adjusted "often" while using the Airband tool.
//
// WHERE WHAT LIVES. The dials are drawn here; what they DO is the engine's, reached through the table and nothing else:
//   - GAIN is the radio's FIRST stage (the status column's read of the GAINS list), turned over that stage's own travel and step, through FOXAPI_OP_SET_GAIN, PACED:
//     at most one ask a slot of 50 ms while the hand is on the dial and the value the hand ended on, once, when it lets go; an ask the radio refuses (it was busy) is tried
//     again at the same pace a few times; under automatic gain the dial is dead and reads AUTO, and with no radio it reads N/A. The AUTO key under it is
//     FOXAPI_OP_SET_DEVICE_AGC (the Source section's "Auto gain" box, which remembers the switch for that radio); on a radio without automatic gain it is drawn, dead, and
//     a press asks for nothing.
//   - SQUELCH is the receiver's threshold (FOXAPI_OP_SET_SQUELCH, which is the op the Radio section's slider and the keys send too) - and while the AIRBAND monitor listens it
//     is the MONITOR's (FOXAPP_OP_AIRBAND, action 4, sent when the hand lets go: a new threshold rebuilds the block and restarts its channels), the receiver's left where it was.
//     The AUTO key under it is FOXAPP_OP_SET_SQUELCH_AUTO and its lamp is fact.squelch.auto: AUTOMATIC squelch is the ENGINE's (engine/engine_squelch_auto.cpp), which also
//     turns it off for a hand on any threshold - so a twist here, the Radio section's slider, the keys, a web page and the Airband section's own slider and key all do, with
//     no window code of their own.
//   - VOLUME is as it has always been (FOXAPI_OP_SET_VOLUME).
//
// WHAT IS HELD HERE, each by tests/test_deck_knobs_app.cpp (the real application behind a fake radio) and tests/test_theme_census.cpp (all the themes): the order GAIN, SQUELCH,
// VOLUME and one size and equal spacing, each AUTO key under its own knob and none on another part of the deck, both meters still on the bar at 1280 x 720; a hand on a dial
// is a manual choice; the knob never asks the radio every frame; and the knob on a radio WITHOUT automatic gain draws the key and asks the radio for nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>   // ImTextCharFromUtf8

#include "core/i18n.hpp"
#include "engine/tune_control.hpp"
#include "gui/deck_parts.hpp"
#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
namespace cmd = cascade::core::cmd;

// --- the engraved captions (drawToolbar's too) ----------------------------------------------------------------------------------------------------

float barTrackedWidth(ImFont* f, float px, const char* text, float track) {
    if (f == nullptr || text == nullptr) { return 0.0f; }
    float w = 0.0f;
    int glyphs = 0;
    for (const char* p = text; *p != '\0';) {
        unsigned int cp = 0;
        const int bytes = std::max(1, ImTextCharFromUtf8(&cp, p, nullptr));
        w += f->CalcTextSizeA(px, FLT_MAX, 0.0f, p, p + bytes).x;
        ++glyphs;
        p += bytes;
    }
    if (glyphs > 1) { w += track * static_cast<float>(glyphs - 1); }
    return w;
}

// THE CUT IS DEEPER THAN IT WAS, AND THAT IS THE OTHER HALF OF "HARD TO SEE". theme.hpp says so in its own header: dark-into-brass is about 2.3:1, "fine for a label at rest
// and not acceptable for a number somebody is trying to read at arm's length". Measured, kEngraved (#3B3529) on this deck's own gradient is 2.11:1 against kBrassMid at the
// bottom of the ramp and 2.62:1 against kBrassShade at the top - the worst contrast anywhere on this window, and it is carrying MASTER, TUNED - HERTZ and VOLUME, which are the
// three words that say what the three instruments on the deck ARE. So the cut is FILLED rather than repainted: the void laid into it at 0.90, which is what a paint-filled
// engraving on a 1960s brass panel actually is, and which measures 3.19:1 against kBrassMid and 3.93:1 against kBrassShade. The lit lip is raised with it, from 0.55 to 0.75,
// because on a real cut it is the highlight along the lower edge that makes the letterform, not the shadow. Nothing else about the treatment changes and no other surface is
// touched: the maker's plate at the foot of the status column keeps the shallow cut, because a maker's plate is decoration and is not read for information.
void barEngrave(ImDrawList* dl, ImVec2 at, float px, const char* text, bool centred, float trackFrac) {
    if (dl == nullptr || text == nullptr || text[0] == '\0') { return; }
    ImFont* f = fonts::legend();
    const float track = px * trackFrac;
    if (centred) { at.x -= barTrackedWidth(f, px, text, track) * 0.5f; }
    const ImU32 lip = theme::withAlpha(theme::kBrassTint, 0.75f);
    // The cut is the deck's own ink (foxsdr-ui/1 "deckInk").
    const ImU32 cut = theme::tone(0x0D, 0x0B, 0x07, 230, theme::ink::DeckInk);
    float x = at.x;
    for (const char* p = text; *p != '\0';) {
        unsigned int cp = 0;
        const int bytes = std::max(1, ImTextCharFromUtf8(&cp, p, nullptr));
        dl->AddText(f, px, ImVec2(x, at.y + 1.0f), lip, p, p + bytes);
        dl->AddText(f, px, ImVec2(x, at.y), cut, p, p + bytes);
        x += f->CalcTextSizeA(px, FLT_MAX, 0.0f, p, p + bytes).x + track;
        p += bytes;
    }
}

namespace {

// The gain stage's travel for a driver that reports none (the Source section's own fallback: app_window.cpp kSoapyGainMinDb / kSoapyGainMaxDb).
constexpr float kFallbackGainMinDb = 0.0f;
constexpr float kFallbackGainMaxDb = 60.0f;

// --- THE DECK'S AUTO KEYS (0.99.74) --------------------------------------------------------------------------------------------------------
//
// One small LATCHED brass key under the GAIN dial and one under the SQUELCH dial, lettered AUTO - the tester: "a switch for Auto Gain" and "a button for AUTO or above the
// noise". It is the function selector's key (benchBankKey: it stays down while it is on, with a phosphor strip lit under it, the display's colour for "this is what is on")
// with one state the selector never has: DEAD. A radio with no automatic gain still shows the key - a control that vanishes when it cannot be used is a control nobody learns -
// engraved dark, with no lamp, and a press does nothing; the tooltip says why.
//
// A real ImGui item, so it is reached with Tab and takes part in the same input arbitration as everything else on the bar. `censusName` names it in the interface census
// ("deck:gainauto", "deck:squelchauto") and is its item id. Returns true on a press that should act (never for a dead key).
bool deckAutoKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool on, bool enabled, const char* tooltip, const char* censusName) {
    if (dl == nullptr || br.x - tl.x < 12.0f || br.y - tl.y < 8.0f) { return false; }
    census::note(censusName);
    census::rect(censusName, tl.x, tl.y, br.x, br.y);
    ImGui::PushID(censusName);
    ImGui::SetCursorScreenPos(tl);
    const bool clicked = ImGui::InvisibleButton("##deckauto", ImVec2(br.x - tl.x, br.y - tl.y));
    // THE LAMP, NOTED (stub campaign B12): "<name>.lit" while the key is on, so a test reads what the key SHOWS and not only what a press sent.
    if (on && enabled) {
        char lit[48];
        std::snprintf(lit, sizeof(lit), "%s.lit", censusName);
        census::note(lit);
    }
    const bool hovered = ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    if (hovered && tooltip != nullptr) { ImGui::SetTooltip("%s", tooltip); }
    ImGui::PopID();

    const float r = theme::kKeyRounding;
    const bool down = enabled && (on || held);
    if (!enabled) {
        // Dead: the well's dark with the brass only in the outline, like benchWordKey's disabled key.
        dl->AddRectFilled(tl, br, theme::kWell, r);
        dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.80f), r, 0, theme::kHairline);
    } else {
        // Proud metal casts a shadow and pressed metal does not: the state indication before any colour is used.
        if (!down) {
            dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f), theme::shadowOf(0x0D, 0x0B, 0x07, 115), r);
        }
        const ImU32 top = down      ? theme::toneHex(0x6E6552, 255, theme::ink::ActiveBgTop)
                          : hovered ? theme::toneMix(0xEF, 0xE7, 0xD2, 255, theme::ink::CtrlTop, theme::ink::ActiveBgTop, 0.35f)
                                    : theme::toneHex(0xD8CFB4, 255, theme::ink::CtrlTop);
        const ImU32 bot = down ? theme::toneHex(0x4A4234, 255, theme::ink::ActiveBgBot) : theme::toneHex(0x8B8069, 255, theme::ink::CtrlBot);
        dl->AddRectFilled(tl, br, bot, r);
        if (br.x - tl.x > r * 2.0f) {
            dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top, top, bot, bot);
        }
        addBenchBevel(dl, tl, br, r, !down);
        // THE LAMP STRIP: lit phosphor under the key that is in, dark glass under the one that is out. Two pixels high and two below the key, where the selector's is three and
        // three: the bar's foot rail stands 3 px above its edge, and at the smallest scale the bar is drawn at the key is only ten pixels tall.
        const ImVec2 sTL(tl.x + 3.0f, br.y + 2.0f);
        const ImVec2 sBR(br.x - 3.0f, br.y + 4.0f);
        dl->AddRectFilled(sTL, sBR, theme::kWell, 1.0f);
        if (on) {
            dl->AddRectFilled(ImVec2(sTL.x - 1.0f, sTL.y - 1.0f), ImVec2(sBR.x + 1.0f, sBR.y + 1.0f), theme::toneHex(0x8FD9A0, 64, theme::ink::ActiveLine), 2.0f);
            dl->AddRectFilled(sTL, sBR, theme::toneHex(0x8FD9A0, 255, theme::ink::ActiveLine), 1.0f);
        }
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f), theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }
    // The word is cut into the brass: ink on metal, never amber. Lettered at the key's height less its edges, never under nine pixels, and fitted to the key's width for a
    // language whose word for AUTO is longer.
    const float px = std::max(9.0f, std::min(fonts::tinyPx(), (br.y - tl.y) - 4.0f));
    addFittedCentred(dl, fonts::ui(), px, tl, br,
                     !enabled ? theme::kInkFaint
                     : on     ? theme::toneHex(0xEFE7D2, 255, theme::ink::ActiveText)
                              : theme::toneHex(0x2A251C, 255, theme::ink::CtrlText),
                     tr("AUTO"), uiscale::px(2.0f), down ? 1.0f : 0.0f, 8.0f);
    return clicked && enabled;
}

}  // namespace

// --- the knob row ----------------------------------------------------------------------------------------------------------------------------

void AppWindow::drawDeckKnobs(ImDrawList* dl, const ImVec2& barTL, const ImVec2& barBR, float scale, float capPx, const CounterLayout& layout) {
    // Reference units to screen pixels, as drawToolbar has them.
    const auto X = [&](float u) { return barTL.x + u * scale; };
    const auto Y = [&](float u) { return barTL.y + u * scale; };
    const auto S = [&](float u) { return u * scale; };
    DeckKnobState& d = deck_;
    const float knobR = S(kDeckKnobR);
    const float knobCy = Y(kDeckKnobCy);
    const float cell = S(kDeckKnobPitch);  // each dial's own width on the bar
    const float cxs[3] = {X(deckGainCx(layout)), X(deckSquelchCx(layout)), X(deckVolumeCx(layout))};
    const char* const caps[3] = {tr("GAIN"), tr("SQUELCH"), tr("VOLUME")};

    // THE CAPTIONS, ONE SIZE FOR ALL THREE and fitted to the cell: SQUELCH is twice the width of GAIN and wider than the 72-unit pitch at the deck's own caption size, and a
    // translated caption is longer again. The order of what gives way: the size first, down to the nine pixels below which an engraving is dirt on the panel; then the
    // letter-spacing, which is decoration; and a word that still does not fit is cut at its own cell's edge, never run into its neighbour's.
    ImFont* capFont = fonts::legend();
    float capSize = capPx;
    float capTrack = 0.24f;
    {
        const float room = cell - S(6.0f);
        for (const float tf : {0.24f, 0.10f, 0.0f}) {
            float sz = capPx;
            for (const char* c : caps) {
                sz = std::min(sz, fitSize(capPx, room, [&](float z) { return barTrackedWidth(capFont, z, c, z * tf); }, 9.0f));
            }
            capSize = sz;
            capTrack = tf;
            bool whole = true;
            for (const char* c : caps) { whole = whole && barTrackedWidth(capFont, sz, c, sz * tf) <= room + 0.5f; }
            if (whole) { break; }
        }
    }
    for (int i = 0; i < 3; ++i) {
        dl->PushClipRect(ImVec2(cxs[i] - cell * 0.5f, barTL.y), ImVec2(cxs[i] + cell * 0.5f, barBR.y), true);
        barEngrave(dl, ImVec2(cxs[i], Y(kDeckKnobCaptionY)), capSize, caps[i], true, capTrack);
        dl->PopClipRect();
    }
    // THE SETTING, IN CREAM, AND DELIBERATELY NOT IN AMBER. This is where the hand has put the control, not something the radio measured, and the palette reserves amber for a
    // reading. Figures, so the monospaced face: the number stops shuffling sideways as the dial turns. Fitted to the cell like the captions: "-120 dB" at the 1.4x readings size
    // is wider than the pitch.
    const auto reading = [&](int i, const char* txt) {
        ImFont* vf = fonts::reading();
        float vpx = std::max(11.0f, fonts::kReadingSize * scale * theme::readingsScale());
        vpx = fitTextPx(vf, vpx, txt, cell - S(6.0f), 9.0f);
        const ImVec2 vs = vf->CalcTextSizeA(vpx, FLT_MAX, 0.0f, txt);
        dl->AddText(vf, vpx, ImVec2(cxs[i] - vs.x * 0.5f, Y(kDeckKnobReadingY)), theme::toneHex(0xD8CFB4, 255, theme::ink::DeckInk), txt);
    };
    const float autoKeyW = S(kDeckAutoKeyW);
    // Never under ten pixels, so its word stays a word at the smallest scale.
    const float autoKeyH = std::max(S(kDeckAutoKeyH), 10.0f);
    const float autoKeyTop = Y(kDeckAutoKeyY);
    const double nowS = ImGui::GetTime();

    // --- GAIN: the radio's first stage -----------------------------------------------------------------------------------------------------
    //
    // The scope deck's GAIN knob is the precedent (drawScopeKnob in drawScopeFace): the FIRST gain stage the device reports, over that stage's own range, set through the
    // engine's SET_GAIN and read back. It refuses to pretend, as that one does: with no radio, or no gain stage, there is nothing to set and it reads N/A; under automatic gain a
    // manual gain would fight the radio, so it is dead and reads AUTO. Either way it is drawn, veiled, and answers nothing.
    {
        refreshStatusLists();   // the first stage, whenever listSeq has moved (the status column's own read; the column may be folded)
        const bool deviceOpen = (uiState_.flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
        const bool agcOn = (uiState_.flags & FOXAPI_RX_DEVICE_AGC) != 0u;
        const bool agcSupported = (uiState_.flags & FOXAPI_RX_AGC_SUPPORTED) != 0u;
        const bool haveGain = deviceOpen && statusGainHave_;
        const bool gainLive = haveGain && !agcOn;
        float loDb = kFallbackGainMinDb;
        float hiDb = kFallbackGainMaxDb;
        if (haveGain && statusGain0Hi_ > statusGain0Lo_) {
            loDb = static_cast<float>(statusGain0Lo_);
            hiDb = static_cast<float>(statusGain0Hi_);
        }
        // WHERE THE DIAL STANDS: the hand's target while the hand is on it or an ask is out, the figure the radio gave back for the last ask until the list shows it, and the
        // radio's readback otherwise (the radio quantises, and a dial that kept showing 21.4 while the tuner sat at 20.7 would be the same lie the antenna combo was fixed for).
        if (d.gainTicket != 0u && nowS - d.gainSetS > 2.0) { d.gainTicket = 0; }   // an ask whose answer never came (a session that was closed)
        float shownDb = loDb;
        if (haveGain) {
            const float listDb = static_cast<float>(statusGain0Db_);
            if (d.gainHeld || d.gainDirty || d.gainTicket != 0u) {
                shownDb = d.gainTargetDb;
            } else if (d.gainReadHold > 0 && std::fabs(listDb - d.gainReadDb) > 0.005f) {
                shownDb = d.gainReadDb;
            } else {
                shownDb = listDb;
            }
        }
        if (d.gainReadHold > 0) { --d.gainReadHold; }
        const float frac = hiDb > loDb ? std::clamp((shownDb - loDb) / (hiDb - loDb), 0.0f, 1.0f) : 0.0f;
        census::note("deck:gain");
        census::rect("deck:gain", cxs[0] - knobR, knobCy - knobR, cxs[0] + knobR, knobCy + knobR);
        const float moved = drawBrassKnob(dl, ImVec2(cxs[0], knobCy), knobR, frac, "##deckgain", gainLive);
        const bool handOn = ImGui::IsItemActive();
        if (ImGui::IsItemHovered()) {
            std::string tip = tr("Gain - the radio's first stage; drag the dial, or scroll over it");
            if (haveGain) {
                tip += "\n";
                tip += statusGain0Name_;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        // THE FRAME THE HAND LANDS ON THE DIAL: the target starts from where the dial stands, so that the frames that follow (which read the target, the hand being on it)
        // accumulate from there and not from whatever the target last held.
        if (gainLive && handOn && !d.gainHeld && !d.gainDirty) { d.gainTargetDb = shownDb; }
        if (moved >= 0.0f && gainLive) {
            const float want = loDb + moved * (hiDb - loDb);
            if (std::fabs(want - shownDb) > 1.0e-4f) {
                d.gainTargetDb = want;
                d.gainDirty = true;
                d.gainRetries = 0;
            }
        }
        d.gainHeld = gainLive && handOn;
        // PACED: at most one ask every kDeckGainPaceS while the dial is held (a native driver can answer "busy", and a dial that moves every frame must not hammer it), and the
        // final value as soon as the hand lets go.
        if (d.gainDirty && !gainLive) {
            d.gainDirty = false;  // automatic gain came on, or the radio went
        } else if (d.gainDirty && (nowS - d.gainSetS >= kDeckGainPaceS || (!handOn && d.gainRetries == 0))) {
            deckAskGain();
        }
        std::string gainTxt;
        if (!haveGain) {
            gainTxt = tr("N/A");
        } else if (agcOn) {
            gainTxt = tr("AUTO");
        } else {
            // In the first stage's OWN unit: over an Airspy it reads "7", the step it is on, not "7 dB" of nothing.
            gainTxt = formatGainValue(static_cast<double>((d.gainHeld || d.gainDirty || d.gainTicket != 0u) ? d.gainTargetDb : shownDb), statusGain0Unit_);
        }
        reading(0, gainTxt.c_str());
        const bool agcOk = deviceOpen && agcSupported;
        if (deckAutoKey(dl, ImVec2(cxs[0] - autoKeyW * 0.5f, autoKeyTop), ImVec2(cxs[0] + autoKeyW * 0.5f, autoKeyTop + autoKeyH), agcOn, agcOk,
                        agcOk ? tr("Automatic gain - the radio sets its own gain") : tr("This radio has no automatic gain"), "deck:gainauto")) {
            // A step of the gain dial still waiting for its turn would only fight the switch. The engine asks the radio, mirrors an accepted change, remembers it for this radio
            // and, on a refusal, leaves the key where it was and puts the driver's words in the Source section's error line.
            d.gainDirty = false;
            uiSubmit(cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, agcOn ? 0 : 1));
        }
    }

    // --- SQUELCH: where the gate opens -----------------------------------------------------------------------------------------------------
    //
    // Over the Radio section slider's own [-120, 0] dB span, so the dial, the slider, the S-meter bar and the keys share one scale. A hand on the dial is a manual choice and the
    // engine puts AUTOMATIC off for it; AUTOMATIC itself moves the dial (the engine sets the threshold, which this reads).
    //
    // WHILE THE AIRBAND MONITOR LISTENS IT IS THE MONITOR'S SQUELCH (the tester: "I really like the Airband tool and find myself adjusting the Squelch often when doing so"): the
    // monitor plays each channel behind a squelch of its own, over -110..0, and the idle receiver's threshold would change nothing that is heard. The dial reads and sets that
    // one, the reading under it is the value in use, and the receiver's is left where it was; the caption is the same word and the tooltip says which it is. The monitor's block is
    // rebuilt with a new threshold on release (the engine restarts its channels), not every frame of a drag.
    {
        const AirbandFacts& ab = airbandFacts();
        const bool onAirband = ab.listening;
        const float sqLo = onAirband ? -110.0f : -120.0f;
        // What the engine says the threshold is, and what the dial shows: the hand's figure until the engine's has caught up (a command is applied on the next pass and read
        // back the frame after).
        const float engineDb = onAirband ? ab.squelchDb : static_cast<float>(uiState_.squelchDb);
        if (d.onAirbandLast != onAirband) {
            d.squelchHold = 0;   // the other dial's figure is not this one's
            d.onAirbandLast = onAirband;
        }
        float sq = engineDb;
        if (d.squelchHold > 0) {
            --d.squelchHold;
            if (std::fabs(engineDb - d.squelchEdit) > 0.05f) { sq = d.squelchEdit; } else { d.squelchHold = 0; }
        }
        const float frac = std::clamp((sq - sqLo) / (0.0f - sqLo), 0.0f, 1.0f);
        census::note("deck:squelch");
        census::rect("deck:squelch", cxs[1] - knobR, knobCy - knobR, cxs[1] + knobR, knobCy + knobR);
        const float moved = drawBrassKnob(dl, ImVec2(cxs[1], knobCy), knobR, frac, "##decksquelch", true);
        const bool handOn = ImGui::IsItemActive();
        if (ImGui::IsItemHovered()) {
            std::string tip = tr("Squelch - drag the dial, or scroll over it");
            if (onAirband) {
                tip += "\n";
                tip += tr("Airband is listening: this sets the Airband squelch");
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (moved >= 0.0f) {
            const float want = std::clamp(sqLo + moved * (0.0f - sqLo), sqLo, 0.0f);
            // Pressing the dial without turning it is not a choice of threshold.
            if (std::fabs(want - sq) > 1.0e-4f) {
                sq = want;
                d.squelchEdit = want;
                d.squelchHold = kDeckSquelchHoldFrames;
                if (onAirband) {
                    d.airbandDirty = true;
                } else {
                    // The one op every hand on the threshold sends: the engine sets it and turns AUTOMATIC off.
                    uiSubmit(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, static_cast<double>(want)));
                }
            }
        }
        // The monitor's new threshold takes effect when the hand lets go (or at once for a wheel notch, which is never held), and is dropped if it stopped listening meanwhile.
        if (d.airbandDirty && !onAirband) {
            d.airbandDirty = false;
        } else if (d.airbandDirty && !handOn) {
            d.airbandDirty = false;
            uiSubmit(cmd::makeNumInt(FOXAPP_OP_AIRBAND, static_cast<double>(d.squelchEdit), 4));
        }
        char sqTxt[16];
        std::snprintf(sqTxt, sizeof(sqTxt), "%.0f dB", static_cast<double>(sq));
        reading(1, sqTxt);
        const bool autoLit = uiFact("fact.squelch.auto") == "1";
        if (deckAutoKey(dl, ImVec2(cxs[1] - autoKeyW * 0.5f, autoKeyTop), ImVec2(cxs[1] + autoKeyW * 0.5f, autoKeyTop + autoKeyH), autoLit, true,
                        autoLit ? tr("Automatic squelch - sits 8 dB above the measured noise floor")
                                : tr("Automatic squelch - follows the noise floor so weak carriers open it and noise does not"),
                        "deck:squelchauto")) {
            uiSubmit(cmd::makeInt(FOXAPP_OP_SET_SQUELCH_AUTO, autoLit ? 0 : 1));
        }
    }

    // --- VOLUME: as it has always been -------------------------------------------------------------------------------------------------------
    {
        census::note("deck:volume");
        census::rect("deck:volume", cxs[2] - knobR, knobCy - knobR, cxs[2] + knobR, knobCy + knobR);
        // (The setting as the engine published it: published as the decimal the float was set to, so the cast back is the float it was.)
        const float volume = static_cast<float>(uiState_.volume);
        const float moved = drawBrassVolumeKnob(dl, ImVec2(cxs[2], knobCy), knobR, volume);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tr("Volume - drag the dial, or scroll over it")); }
        if (moved >= 0.0f) { uiSubmit(cmd::makeNum(FOXAPI_OP_SET_VOLUME, moved)); }
        char vtxt[16];
        std::snprintf(vtxt, sizeof(vtxt), "%.2f", static_cast<double>(volume));
        reading(2, vtxt);
    }
}

// THE RADIO IS ASKED for the stage's new figure: rounded to the stage's own step from its own minimum, inside its range, so a drag asks for a value the radio has. The ask is
// the engine's SET_GAIN, whose answer (deckOnAnswer) says whether the radio took it.
void AppWindow::deckAskGain() {
    DeckKnobState& d = deck_;
    d.gainSetS = ImGui::GetTime();
    const bool deviceOpen = (uiState_.flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
    const bool agcOn = (uiState_.flags & FOXAPI_RX_DEVICE_AGC) != 0u;
    if (!deviceOpen || !statusGainHave_ || agcOn) {
        d.gainDirty = false;
        return;
    }
    double db = static_cast<double>(d.gainTargetDb);
    if (statusGain0Hi_ > statusGain0Lo_) {
        db = std::clamp(db, statusGain0Lo_, statusGain0Hi_);
        if (statusGain0Step_ > 0.0) {
            db = std::clamp(statusGain0Lo_ + std::round((db - statusGain0Lo_) / statusGain0Step_) * statusGain0Step_, statusGain0Lo_, statusGain0Hi_);
        }
    }
    const std::uint64_t ticket = uiSubmitTicket(cmd::makeText(FOXAPI_OP_SET_GAIN, statusGain0Name_, 0, 0, db));
    if (ticket == 0u) { return; }   // the session was full (BUSY): still dirty, asked again at the next slot
    d.gainDirty = false;
    d.gainTicket = ticket;
    d.gainSentDb = static_cast<float>(db);
}

// THE ANSWER to the dial's ask: accepted - the radio's own readback is what the dial shows until the list has it (the radio quantises); refused (the radio was busy) - asked
// again at the dial's pace, at most kDeckGainRetries times, DROPPED WITHOUT A WORD: the Source section's error line is not for a dial that moves every frame. The dial then goes
// back to the radio's own readback.
void AppWindow::deckOnAnswer(const FoxCommandResult& r) {
    DeckKnobState& d = deck_;
    if (d.gainTicket == 0u || r.ticket != d.gainTicket) { return; }   // the Source section's slider's, or an old one
    d.gainTicket = 0;
    if (r.status == FOXAPI_OK || r.status == FOXAPI_NO_CHANGE) {
        d.gainRetries = 0;
        d.gainReadDb = static_cast<float>(r.applied[0]);
        d.gainReadHold = kDeckSquelchHoldFrames;
        return;
    }
    if (++d.gainRetries > kDeckGainRetries) {
        d.gainDirty = false;
        d.gainRetries = 0;
    } else if (!d.gainDirty && std::fabs(d.gainTargetDb - d.gainSentDb) < 0.5f * static_cast<float>(statusGain0Step_ > 0.0 ? statusGain0Step_ : 1.0)) {
        d.gainDirty = true;   // nothing newer has been asked for meanwhile: ask again
    }
}

}  // namespace cascade::gui
