// plugin_store_view.cpp - the PLUGIN STORE window, and the shared DATA PLATE.
//
// Read the header first: it carries the division of labour with the FITTED
// MODULES window and, more importantly, the one claim from the design that
// this file refuses to draw.
//
// THE BENCH VOCABULARY. scope_face.hpp is the shared vocabulary and everything
// in it that fits is used here - bevels, rails, group captions, lamps, the
// drum well. What it does not have and this window needs - a recessed WELL, a
// LABELLED brass key, a two-position ROCKER, a selector SEGMENT, a HATCH and a
// NOTE - is built in the anonymous namespace below, exactly as map_view.cpp
// had to build them for the satellites window. They are duplicated rather than
// promoted because scope_face.hpp is not this agent's file to change; when a
// third window wants them, that is the moment to promote all three copies into
// it rather than to make a fourth.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/plugin_store_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "gui/fonts.hpp"
#include "gui/page_geometry.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
#include "gui/ui_scale.hpp"
#include "imgui.h"

namespace cascade::gui {
namespace {

// --- measurement --------------------------------------------------------------

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

float faceH(ImFont* f, float px) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y; }

float wrapH(ImFont* f, float px, float wrapWidth, const char* s) {
    if (s == nullptr || s[0] == '\0') { return 0.0f; }
    if (wrapWidth < 16.0f) { return faceH(f, px); }
    return f->CalcTextSizeA(px, FLT_MAX, wrapWidth, s).y;
}

// A figure is a figure only if it is made of figures. fonts.hpp is narrow
// about this for a measured reason - Nova Mono's capitals merge into solid
// blocks below about 20px - so the monospaced face is chosen by TESTING the
// string rather than by a call site's opinion of what it holds. A version
// string ("1.4.2") and a size ("2.10") are figures; "2.1 MB" is not, because
// the unit is a word.
bool allFigures(const char* s) {
    if (s == nullptr || s[0] == '\0') { return false; }
    for (const char* p = s; *p != '\0'; ++p) {
        const bool digit = (*p >= '0' && *p <= '9');
        if (!digit && *p != '.' && *p != '-' && *p != ':') { return false; }
    }
    return true;
}

ImFont* faceForValue(const char* s) {
    return allFigures(s) ? fonts::reading() : fonts::ui();
}

// THE ONE SIZE EVERY SENTENCE IN THIS WINDOW IS SET IN, through the public
// accessor so a test and the drawing cannot disagree about it. See
// storeProsePx() at the foot of this file for why it is no longer the tiny
// engraving - and note that every measured height and every key width in here
// already derives from it, which is what made the raise one change rather than
// a sweep of literals.
float prose() { return cascade::gui::px(storeProsePx()); }

// --- the vocabulary this window adds ------------------------------------------

// The recessed bay a group of controls sits in: dark enamel cut into the
// panel, a brass lip around it and the bevel lit from below, which is what
// makes it read as a hole rather than as a dark rectangle.
void addDeckWell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < cascade::gui::px(8.0f) ||
        br.y - tl.y < cascade::gui::px(8.0f)) {
        return;
    }
    const float r = theme::kPanelRounding;
    dl->AddRectFilled(tl, br, theme::kEnamelDark, r);
    if (br.x - tl.x > r * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y),
                                    theme::kEnamelDark, theme::kEnamelDark, theme::kWell,
                                    theme::kWell);
    }
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassBright, 0.75f), r, 0, 2.0f);
    addBenchBevel(dl, tl, br, r, false);
}

// The inner box a block of the data plate sits in: the well's own floor, one
// hairline in. Flat rather than gradient, so a box inside a well does not read
// as a second well.
void addPlateBox(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < cascade::gui::px(8.0f) ||
        br.y - tl.y < cascade::gui::px(8.0f)) {
        return;
    }
    dl->AddRectFilled(tl, br, theme::kWell, theme::kKeyRounding);
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.9f), theme::kKeyRounding, 0,
                theme::kHairline);
}

// A labelled brass key, one or two lines. Disabled draws it drained and
// refuses the click - and the sentence saying WHY lives beside it, because a
// greyed key with no explanation is the fault this redesign exists to remove.
bool drawDeckKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* line1,
                 const char* line2, bool enabled, const char* id) {
    if (dl == nullptr || br.x - tl.x < cascade::gui::px(8.0f) ||
        br.y - tl.y < cascade::gui::px(8.0f)) {
        return false;
    }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("##key", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool focused = ImGui::IsItemFocused();
    ImGui::EndDisabled();
    ImGui::PopID();

    const float r = theme::kKeyRounding;
    if (!enabled) {
        dl->AddRectFilled(tl, br, theme::kWell, r);
        dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.80f), r, 0,
                    theme::kHairline);
    } else {
        if (!held) {
            // Proud metal casts a shadow; a pressed key does not. That one
            // difference is the state indication before any colour is used.
            dl->AddRectFilled(ImVec2(tl.x + cascade::gui::px(1.0f), tl.y + cascade::gui::px(2.0f)),
                              ImVec2(br.x + cascade::gui::px(1.0f), br.y + cascade::gui::px(2.0f)),
                              theme::withAlpha(theme::kVoid, 0.45f), r);
        }
        const ImU32 top = held ? theme::kBrassMid : (hovered ? theme::kIvory : theme::kCream);
        const ImU32 bot = held ? theme::kBrassDark : theme::kBrassBright;
        dl->AddRectFilled(tl, br, bot, r);
        if (br.x - tl.x > r * 2.0f) {
            dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y), top,
                                        top, bot, bot);
        }
        addBenchBevel(dl, tl, br, r, !held);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - cascade::gui::px(2.0f), tl.y - cascade::gui::px(2.0f)),
                    ImVec2(br.x + cascade::gui::px(2.0f), br.y + cascade::gui::px(2.0f)),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }

    // Engraved into brass, which the palette's rule allows for a caption on
    // metal and forbids for a reading on glass. A dead key letters in a muted
    // ink instead, so it reads as unavailable rather than as unlabelled.
    //
    // MUTED, NOT FAINT. A disabled key's ground is kWell; kInkFaint on it is
    // about 4:1, and this window has three keys that spend most of their life
    // disabled - FIT on a module that cannot be fitted, ALREADY FITTED, and
    // CHECK NOW before a source is set - so the dead label is the one a user
    // most often has to read. kInkMuted is about 6:1 there and is still a
    // clear step below the cream of a live key.
    ImFont* f = fonts::ui();
    const float px = prose();
    const ImU32 ink = enabled ? theme::kEnamel : theme::kInkMuted;
    const float lh = faceH(f, px);
    const int lines = (line2 != nullptr && line2[0] != '\0') ? 2 : 1;
    float y = (tl.y + br.y) * 0.5f - lh * static_cast<float>(lines) * 0.5f +
              (held ? cascade::gui::px(1.0f) : 0.0f);
    const float cx = (tl.x + br.x) * 0.5f;
    dl->AddText(f, px, ImVec2(cx - textW(f, px, line1) * 0.5f, y), ink, line1);
    if (lines == 2) {
        y += lh;
        dl->AddText(f, px, ImVec2(cx - textW(f, px, line2) * 0.5f, y), ink, line2);
    }
    return pressed;
}

// One rocker row: the switch, its label plate and a right-aligned count. The
// paddle's POSITION says which way it is thrown - up for on, down for off - so
// the control is readable in a greyscale photograph and by the roughly one man
// in twelve for whom colour alone is not a signal.
//
// AT AN EXPLICIT SIZE, for the same reason drawNoteAt() takes one: THE
// CONTROL DECK's compact mode (see the note above `compact` in
// PluginStoreView::draw()) needs the SHOW well's rockers a size down on the
// narrowest real bodies this window has been measured on, where even a
// single column of six rows costs more height than the whole deck is allowed
// - see the note above `rockerPx` in draw() for the actual figures. Still
// never smaller than fonts::kTinySize, the app's own floor for "still has to
// be readable".
bool drawRockerRowAt(ImDrawList* dl, const ImVec2& tl, float width, float rowH,
                     const char* label, const char* trailing, bool on, const char* id,
                     float px) {
    if (dl == nullptr || width < cascade::gui::px(50.0f) || rowH < cascade::gui::px(10.0f)) {
        return false;
    }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##rocker", ImVec2(width, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();

    const float rw = cascade::gui::px(16.0f);
    const ImVec2 rTL(tl.x, tl.y + cascade::gui::px(1.0f));
    const ImVec2 rBR(tl.x + rw, tl.y + rowH - cascade::gui::px(1.0f));
    dl->AddRectFilled(rTL, rBR, theme::kVoid, theme::kKeyRounding);
    dl->AddRect(rTL, rBR, theme::withAlpha(theme::kBrassMid, 0.9f), theme::kKeyRounding, 0,
                theme::kHairline);
    const float ph = (rBR.y - rTL.y) * 0.42f;
    const ImVec2 pTL(rTL.x + cascade::gui::px(2.0f),
                     on ? rTL.y + cascade::gui::px(2.0f) : rBR.y - cascade::gui::px(2.0f) - ph);
    const ImVec2 pBR(rBR.x - cascade::gui::px(2.0f), pTL.y + ph);
    dl->AddRectFilled(pTL, pBR, on ? theme::kCream : theme::kBrassMid, 1.0f);
    addBenchBevel(dl, pTL, pBR, 1.0f, true);

    ImFont* f = fonts::ui();
    const float lw = textW(f, px, label);
    const float lh = faceH(f, px);
    const ImVec2 lTL(tl.x + rw + cascade::gui::px(7.0f),
                     tl.y + (rowH - lh - cascade::gui::px(5.0f)) * 0.5f);
    const ImVec2 lBR(lTL.x + lw + cascade::gui::px(12.0f), lTL.y + lh + cascade::gui::px(5.0f));
    if (on) {
        dl->AddRectFilled(lTL, lBR, theme::kBrassBright, theme::kKeyRounding);
        addBenchBevel(dl, lTL, lBR, theme::kKeyRounding, true);
    } else {
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kBrassDark, 0.9f), theme::kKeyRounding,
                    0, theme::kHairline);
    }
    dl->AddText(f, px, ImVec2(lTL.x + cascade::gui::px(6.0f), lTL.y + cascade::gui::px(2.0f)),
                on ? theme::kEnamel : theme::kCream, label);
    if (hovered) {
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kBrassBright, 0.7f),
                    theme::kKeyRounding, 0, theme::kHairline);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - cascade::gui::px(2.0f), tl.y - cascade::gui::px(1.0f)),
                    ImVec2(tl.x + width + cascade::gui::px(2.0f), tl.y + rowH + cascade::gui::px(1.0f)),
                    theme::kBrassBright, theme::kKeyRounding, 0, theme::kHairline);
    }
    // HOW MANY ROWS THIS SWITCH IS HOLDING BACK, on the switch itself. A
    // filter that hides things without saying how many is how a user comes to
    // believe the catalogue is short.
    if (trailing != nullptr && trailing[0] != '\0') {
        ImFont* rf = faceForValue(trailing);
        const float rW = textW(rf, px, trailing);
        const float rx = tl.x + width - rW;
        if (rx > lBR.x + cascade::gui::px(6.0f)) {
            dl->AddText(rf, px, ImVec2(rx, tl.y + (rowH - faceH(rf, px)) * 0.5f),
                        on ? theme::kAmber : theme::kInkFaint, trailing);
        }
    }
    return pressed;
}

// One segment of a selector. The selected one is a key PRESSED IN - the idiom
// drawBenchKey uses for an engaged function key - rather than a coloured tab:
// rust in this palette means trouble, and a sort order is not trouble.
bool drawSegment(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                 bool selected, const char* id) {
    if (dl == nullptr || br.x - tl.x < cascade::gui::px(6.0f)) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##seg", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();

    const float r = theme::kKeyRounding;
    if (selected) {
        dl->AddRectFilled(tl, br, theme::kBrassDark, r);
        dl->AddRectFilledMultiColor(tl, ImVec2(br.x, tl.y + (br.y - tl.y) * 0.45f),
                                    theme::withAlpha(theme::kVoid, 0.55f),
                                    theme::withAlpha(theme::kVoid, 0.55f),
                                    theme::withAlpha(theme::kVoid, 0.0f),
                                    theme::withAlpha(theme::kVoid, 0.0f));
    } else {
        dl->AddRectFilled(ImVec2(tl.x + cascade::gui::px(1.0f), tl.y + cascade::gui::px(2.0f)),
                          ImVec2(br.x + cascade::gui::px(1.0f), br.y + cascade::gui::px(2.0f)),
                          theme::withAlpha(theme::kVoid, 0.40f), r);
        dl->AddRectFilled(tl, br, hovered ? theme::kBrassBright : theme::kBrassMid, r);
    }
    addBenchBevel(dl, tl, br, r, !selected);
    if (focused) {
        dl->AddRect(ImVec2(tl.x - cascade::gui::px(2.0f), tl.y - cascade::gui::px(2.0f)),
                    ImVec2(br.x + cascade::gui::px(2.0f), br.y + cascade::gui::px(2.0f)),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }
    ImFont* f = fonts::ui();
    const float px = prose();
    dl->AddText(f, px,
                ImVec2((tl.x + br.x) * 0.5f - textW(f, px, label) * 0.5f,
                       (tl.y + br.y) * 0.5f - faceH(f, px) * 0.5f +
                           (selected ? cascade::gui::px(1.0f) : 0.0f)),
                selected ? theme::kCream : theme::kEnamel, label);
    return pressed;
}

// A HATCHED VALUE: there is no source for this figure, and here is the space
// it would occupy if there were. Diagonal ruling rather than a zero, because
// "0 bytes" and "nobody told us the size" are opposite statements.
void addHatch(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < cascade::gui::px(4.0f) ||
        br.y - tl.y < cascade::gui::px(4.0f)) {
        return;
    }
    dl->PushClipRect(tl, br, true);
    const float h = br.y - tl.y;
    const ImU32 col = theme::withAlpha(theme::kInkMuted, 0.24f);
    for (float x = tl.x - h; x < br.x; x += cascade::gui::px(6.0f)) {
        dl->AddLine(ImVec2(x, br.y), ImVec2(x + h, tl.y), col, 2.0f);
    }
    dl->PopClipRect();
}

// A note: a coloured rule down the left, a wash behind it and the sentence
// itself. `accent` carries the meaning - phosphor for something working, gold
// for something the user should look at, rust for something refused.
//
// AT AN EXPLICIT SIZE, because THE CONTROL DECK's compact mode (see the note
// above `compact` in PluginStoreView::draw()) draws its longest explanatory
// paragraphs a size down from the window's own prose when the full size would
// push the deck past gui::pageDeckHeightCap() - never the interactive
// controls beside them, which stay legible at the size every key and label in
// this window uses. noteHeight()/drawNote() below are the ordinary case,
// unchanged, calling straight through at prose().
float noteHeightAt(float width, const char* text, float px) {
    ImFont* f = fonts::ui();
    if (text == nullptr || text[0] == '\0') { return 0.0f; }
    return wrapH(f, px, width - cascade::gui::px(12.0f), text) + cascade::gui::px(9.0f);
}

void drawNoteAt(ImDrawList* dl, const ImVec2& tl, float width, ImU32 accent, const char* text,
               float px) {
    if (dl == nullptr || width < cascade::gui::px(30.0f) || text == nullptr ||
        text[0] == '\0') {
        return;
    }
    ImFont* f = fonts::ui();
    const float h = noteHeightAt(width, text, px);
    dl->AddRectFilled(tl, ImVec2(tl.x + width, tl.y + h), theme::withAlpha(accent, 0.10f));
    dl->AddRectFilled(tl, ImVec2(tl.x + cascade::gui::px(2.0f), tl.y + h), accent);
    dl->AddText(f, px, ImVec2(tl.x + cascade::gui::px(9.0f), tl.y + cascade::gui::px(4.0f)),
                accent, text, nullptr, width - cascade::gui::px(12.0f));
}

// THE ORDINARY CASE - every call site in this file bar THE CONTROL DECK's
// compact-mode texts - at the window's own prose size, unchanged from before
// noteHeightAt()/drawNoteAt() existed.
float noteHeight(float width, const char* text) { return noteHeightAt(width, text, prose()); }

void drawNote(ImDrawList* dl, const ImVec2& tl, float width, ImU32 accent, const char* text) {
    drawNoteAt(dl, tl, width, accent, text, prose());
}

// "3 OF 11 SHOWN": the figures in the monospaced face and in amber because
// they are readings; the words beside them in the ui face and in ink because
// they are not. One helper so the two never get transcribed the other way
// round at a second call site.
void drawCountLine(ImDrawList* dl, const ImVec2& at, int n, int m, const char* trail) {
    ImFont* uf = fonts::ui();
    ImFont* rf = fonts::reading();
    const float px = prose();
    char nBuf[16];
    char mBuf[16];
    std::snprintf(nBuf, sizeof nBuf, "%d", n);
    std::snprintf(mBuf, sizeof mBuf, "%d", m);
    const float base = at.y;
    const float uOff = base + (faceH(rf, px) - faceH(uf, px)) * 0.5f;
    float x = at.x;
    // THE WORDS ARE MUTED, NOT FAINT. The two figures are the reading and keep
    // the amber; the words between and after them are what say WHAT was
    // counted, and "3 OF 11 MODULES SHOWN" with the words unreadable is a pair
    // of numbers about nothing.
    dl->AddText(rf, px, ImVec2(x, base), theme::kAmber, nBuf);
    x += textW(rf, px, nBuf);
    dl->AddText(uf, px, ImVec2(x, uOff), theme::kInkMuted, " OF ");
    x += textW(uf, px, " OF ");
    dl->AddText(rf, px, ImVec2(x, base), theme::kAmber, mBuf);
    x += textW(rf, px, mBuf);
    dl->AddText(uf, px, ImVec2(x + cascade::gui::px(4.0f), uOff), theme::kInkMuted, trail);
}

float countLineHeight() {
    return std::max(faceH(fonts::reading(), prose()), faceH(fonts::ui(), prose()));
}

// --- the plate's contents ------------------------------------------------------

// One cell of the facts grid. `hatched` is the honest empty: no source for
// this value, drawn as ruling with the reason lettered over it.
struct PlateFact {
    const char* key;
    std::string value;
    bool hatched = false;
    ImU32 tone = theme::kIvory;
};

// One row of the reach list. `outward` marks a capability that reaches beyond
// the host - asking to move the receiver, or fetching from a server - which is
// the only distinction the declaration honestly supports.
struct ReachRow {
    const char* key;
    std::string detail;
    bool outward = false;
};

std::string bytesText(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof buf, "%.2f MB", static_cast<double>(bytes) / 1.0e6);
    } else if (bytes >= 1000ull) {
        std::snprintf(buf, sizeof buf, "%.0f kB", static_cast<double>(bytes) / 1.0e3);
    } else {
        // EXACT UNDER A KILOBYTE, because rounding gets to "0 kB" - which is
        // the one figure this plate must never print for something that is
        // there. A 33-byte file that is not a module at all read as nothing at
        // all, which is the same conflation the hatching exists to prevent.
        std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// THE THREE BITS THAT MAKE A MODULE SOMETHING SIGNAL CAN BE ROUTED TO.
// PluginRunner creates an instance for a decoder, an I/Q decoder or an image
// decoder and for nothing else, so a module with none of them is fed nothing
// by design - the same constant the FITTED MODULES window derives its NoSignal
// state from, and the reason both windows can say so in the same words.
constexpr std::uint32_t kSignalCaps =
    CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;

// What this side can honestly say about a module on this machine. See the
// header: STARTED is the coarser half of the fitted window's FED / NOT FED,
// never a contradiction of it, because nothing on this side is told whether
// anything is reaching the module.
enum class PlateState { NotFitted, Refused, Stopped, NoSignal, Started };

PlateState plateState(const ModulePlate& m) {
    if (!m.fitted) { return PlateState::NotFitted; }
    if (!m.loaded) { return PlateState::Refused; }
    if (!m.running) { return PlateState::Stopped; }
    // Only a module whose declaration was actually read can be known to take
    // no signal. Without it this is a module that is started and nothing more,
    // which is what the word says.
    if (m.haveCapabilities && (m.capabilities & kSignalCaps) == 0u) {
        return PlateState::NoSignal;
    }
    return PlateState::Started;
}

// NEVER READ IS NOT THE SAME AS NOT STATED, and the difference is three
// separate claims about a module nobody has read. A file the host refused
// before validatePluginDesc accepted it never had its name, version, author or
// licence copied out (plugin_host.cpp:232-249), so all four arrive here empty
// - and "not stated" and "none declared" would report the maker's silence
// where the truth is our own ignorance. One phrase, used for every such cell.
const char* kNotRead = "not read";

std::vector<PlateFact> collectFacts(const ModulePlate& m) {
    std::vector<PlateFact> f;
    // A cell whose source was never read is hatched and lettered faint,
    // whatever the cell would otherwise have said.
    const bool read = m.haveDescriptor;

    PlateFact maker{"MAKER", m.maker, false, theme::kIvory};
    if (!read) {
        maker.value = kNotRead;
        maker.hatched = true;
        maker.tone = theme::kInkFaint;
    } else if (m.maker.empty()) {
        maker.value = "not stated";
        maker.hatched = true;
    }
    f.push_back(maker);

    // NOT DIMMED WHEN ABSENT, and this one matters: the host refuses to LOAD a
    // module that declares no licence, and the store refuses to install a
    // catalogue entry without one. "No licence" is a decision, not a blank -
    // but only where a licence was actually looked for. On a refused file the
    // gold "none declared" would be an accusation nobody checked.
    PlateFact lic{"LICENCE", m.licence, false, theme::kIvory};
    if (!read) {
        lic.value = kNotRead;
        lic.hatched = true;
        lic.tone = theme::kInkFaint;
    } else if (m.licence.empty()) {
        lic.value = "none declared";
        lic.hatched = true;
        lic.tone = theme::kGold;
    }
    f.push_back(lic);

    PlateFact ver{"VERSION", m.version, false, theme::kAmber};
    if (!read || m.version.empty()) {
        ver.value = read ? "not stated" : kNotRead;
        ver.hatched = true;
        ver.tone = read ? theme::kIvory : theme::kInkFaint;
    }
    f.push_back(ver);

    // "ON DISK" WHEN THE FIGURE WAS MEASURED HERE, "DOWNLOAD" when a catalogue
    // published it. See ModulePlate::sizeIsOnDisk: one caption for both was a
    // description of a transfer for modules that were already installed, and a
    // description of an impossible one on a platform where every module is
    // compiled into the application.
    PlateFact size{m.sizeIsOnDisk ? "ON DISK" : "DOWNLOAD", {}, false, theme::kAmber};
    if (m.haveSizeBytes) {
        size.value = bytesText(m.sizeBytes);
    } else {
        // The catalogue's size is advisory and OPTIONAL, and there is no
        // published-date field anywhere in the record, so neither is invented.
        size.value = "not stated";
        size.hatched = true;
        size.tone = theme::kIvory;
    }
    f.push_back(size);

    PlateFact abi{"PLUGIN ABI", {}, false, theme::kIvory};
    if (!m.haveAbi) {
        // abiVersion 0 in a manifest means "not recorded", which the retirement
        // predicate treats as UNKNOWN and never as a mismatch. Same rule here.
        abi.value = "not recorded";
        abi.hatched = true;
    } else {
        char buf[64];
        if (m.abiVersion == m.hostAbiVersion) {
            std::snprintf(buf, sizeof buf, "%u, matches this build", m.abiVersion);
            abi.tone = theme::kIvory;
        } else {
            std::snprintf(buf, sizeof buf, "%u, this build needs %u", m.abiVersion,
                          m.hostAbiVersion);
            abi.tone = theme::kGold;
        }
        abi.value = buf;
    }
    f.push_back(abi);

    // FITTED IS NOT THE SAME AS STARTED, a module that was refused is a third
    // thing again, and a module that takes no signal at all is a fourth - so
    // each gets its own words rather than one lamp the user has to interpret.
    //
    // THIS LINE NO LONGER SAYS "RUNNING". It used to letter "fitted and
    // running" in phosphor for anything loaded and not stopped, which put a
    // working light on a decoder that might be fed nothing at all - a claim
    // this side cannot test, because it is handed no runner and no receiver.
    // It says what it knows, in the same five words the row and the lamp use.
    PlateFact state{"ON THIS MACHINE", {}, false, theme::kInkMuted};
    state.tone = moduleStateColour(m);
    switch (plateState(m)) {
        case PlateState::NotFitted: state.value = "not fitted"; break;
        case PlateState::Refused: state.value = "fitted, refused"; break;
        case PlateState::Stopped: state.value = "fitted, stopped"; break;
        case PlateState::NoSignal: state.value = "fitted, takes no signal"; break;
        case PlateState::Started:
            // STARTED, NOT DECODING. Whether anything reaches it is on the
            // FITTED MODULES window, which is handed the runner and the
            // receiver; saying more here would be the two windows disagreeing.
            state.value = "fitted and started";
            break;
    }
    f.push_back(state);

    if (!m.fileName.empty()) { f.push_back({"FILE", m.fileName, false, theme::kInkMuted}); }
    if (!m.platforms.empty()) {
        f.push_back({"BUILDS FOR", m.platforms, false, theme::kInkMuted});
    }
    if (!m.retirementFloor.empty()) {
        // Empty is the normal case and means NO floor. It is only ever drawn
        // when the catalogue positively published one.
        f.push_back({"RETIRED BELOW", m.retirementFloor, false, theme::kGold});
    }
    return f;
}

std::vector<ReachRow> collectReach(const ModulePlate& m) {
    std::vector<ReachRow> r;
    if (!m.haveCapabilities) { return r; }
    const std::uint32_t c = m.capabilities;
    if ((c & CASCADE_CAP_DECODER) != 0u) {
        r.push_back({"Audio decoder", "Fed the demodulated audio the speakers get.", false});
    }
    if ((c & CASCADE_CAP_IQ_DECODER) != 0u) {
        r.push_back({"I/Q decoder", "Fed complex baseband straight from the receiver.",
                     false});
    }
    if ((c & CASCADE_CAP_IMAGE_DECODER) != 0u) {
        r.push_back({"Image decoder", "Fed samples; returns pictures the host displays.",
                     false});
    }
    if ((c & CASCADE_CAP_AUDIO_OUT) != 0u) {
        // REPLACES, and the word is the whole row. This is not a module that
        // adds a sound to the receiver's: while it is decoding, what the
        // speakers play is the module's and the demodulated audio is not
        // there at all - which is exactly what a user who has just fitted a
        // DAB decoder and can no longer hear the band needs to have been told
        // before it happens.
        r.push_back({"Plays sound through FoxSDR",
                     "Replaces the receiver's audio while it is decoding.", false});
    }
    if ((c & CASCADE_CAP_TRACK_SOURCE) != 0u) {
        r.push_back({"Map targets", "Publishes positions the host draws on its map.",
                     false});
    }
    if ((c & CASCADE_CAP_PANEL) != 0u) {
        r.push_back({"A window of its own", "Rows and controls the host draws for it.",
                     false});
    }
    if ((c & CASCADE_CAP_INSTRUMENT) != 0u) {
        r.push_back({"An instrument of its own",
                     "A face the host draws as a piece of equipment, fed by the module.",
                     false});
    }
    if ((c & CASCADE_CAP_PRESET) != 0u) {
        // Worth its own row precisely because it looks like tuning and is not.
        r.push_back({"Presets", "Publishes where it listens. A suggestion - pressing one "
                                "is the user tuning, not the module.",
                     false});
    }
    if ((c & CASCADE_CAP_HOST_CLIENT) != 0u) {
        std::string d;
        if (!m.haveTuneGrant) {
            d = "Refused unless you grant it, per module. This grant is the one "
                "permission the console actually enforces.";
        } else if (m.tuneGranted) {
            d = "GRANTED. It may retune the receiver on its own, without asking again.";
        } else {
            d = "Not granted, so every request to retune is answered DENIED.";
        }
        r.push_back({"Can ask to move the receiver", d, true});
    }
    if ((c & CASCADE_CAP_BASEMAP) != 0u) {
        // NOT "a server you point it at", which is what this row used to say.
        // CascadeBasemapApi carries no server field and there is no setting in
        // this console that aims a basemap module anywhere: the host asks for
        // the tile at (z, x, y) and the module answers it from wherever it
        // likes. Handing the user a control they have not got, on the one row
        // whose job is to warn them this capability reaches outward, is the
        // worst place in the console to do it.
        r.push_back({"Map imagery", "Supplies the map tiles from whatever source it chose "
                                    "- which may be an online tile server. Nothing here "
                                    "points it at one.",
                     true});
    }
    if ((c & CASCADE_CAP_TRACK_INFO) != 0u) {
        r.push_back({"Target look-up", "Looks up who a target is, from whatever source it "
                                        "chose - which may be an online service.",
                     true});
    }
    if (r.empty()) {
        // A descriptor must declare at least one KNOWN bit to load at all, so
        // both of these are states the caller had to construct: no bits at
        // all, or only bits this build has never heard of. They are different
        // facts and get different words rather than one shrug.
        if (m.capabilities == 0u) {
            r.push_back({"Declares nothing", "The record carries no capability bits.",
                         false});
        } else {
            r.push_back({"Declares a capability this build does not know",
                         "The module was built against a newer host.", true});
        }
    }
    return r;
}

// THE SENTENCE THE DESIGN GOT WRONG, corrected here and stated once.
//
// The mock says the reach list is "enforced by the console - a module cannot
// take anything not on this list". It is not. Plugins load in-process
// (LoadLibraryExW / dlopen), there is no sandbox and no permission model, and
// the CASCADE_CAP_* bits say what a module PROVIDES rather than what it may
// take. Printing the mock's sentence would hand the user a guarantee on the
// exact card - unverified maker, no licence - where they would lean on it
// hardest.
const char* kReachLead =
    "Declared by the maker, not enforced. A fitted module is loaded into this "
    "application's own process and runs with every privilege the application has: "
    "there is no sandbox and no permission model. This list is what the module says "
    "it PROVIDES, not a limit on what it can take.";

const char* kReachUnknown =
    "The catalogue index carries no capability field, so what this module declares is "
    "not known until it is fitted. Fitting it is what fills this in.";

// AN EMPTY LIST MEANS TWO DIFFERENT THINGS AND MUST NOT BE DRAWN ONE WAY.
// Above: a catalogue row nobody has fitted, whose declaration has never been
// read. Here: a file that IS fitted and that the host did not accept - so it
// reaches nothing at this moment because it is not loaded, which is not the
// same as a module that asks for nothing. Saying "not declared until it is
// fitted" of it would be false twice over: it is fitted, and its silence is
// the refusal's, not the module's.
//
// It does NOT say the descriptor was never read, because that is only true of
// some refusals - a module the duplicate resolver turned off was read in full
// first. What is true of every one of them is that no capability list reached
// this panel and none of the module is loaded.
const char* kReachRefused =
    "Not known here, and nothing is routed to it. This file is fitted and the host did "
    "not accept it, so no capability list reached this panel and none of the module is "
    "loaded. That is not the same as a module which declares nothing.";

const char* kReachTuneNote =
    "The tune grant above is the one permission this console does enforce: without it "
    "every request to retune is refused. Nothing else in the list is a gate.";

const char* kReachNoTuneNote =
    "The one permission this console enforces is the per-module tune grant, and this "
    "module does not ask for it. Nothing else in the list is a gate.";

// One pass that both measures and draws, so the two can never drift apart.
float layoutPlate(ImDrawList* dl, const ImVec2& tl, float width, const ModulePlate& m,
                  bool draw) {
    const float kBoxPad = cascade::gui::px(12.0f);
    const float kBoxGap = cascade::gui::px(10.0f);

    ImFont* uf = fonts::ui();
    ImFont* lf = fonts::legend();
    const float tiny = prose();
    // THE NAME LINE TAKES THE SAME LARGEST SIZE as the prose under it and is
    // told apart by its FACE - Georgia Bold against Georgia Regular - rather
    // than by a second figure. One size for the page is what makes "go bigger
    // on the font" one edit; a heading size on top of it would be a second
    // number to keep in step with the first.
    const float uiPx = prose();
    const float tinyH = faceH(uf, tiny);
    const float legH = faceH(lf, tiny);
    const float inner = width - kBoxPad * 2.0f;
    if (inner < cascade::gui::px(60.0f)) { return 0.0f; }
    // THE PLATE'S NAME WRAPS TOO, and for the same reason the row's does: this
    // column is a third of the window, "406 MHz Distress Beacon Decoder (EPIRB
    // / ELT / PLB)" does not fit across it at any size worth reading, and the
    // child that holds the plate simply CUT it - the plate's heading read "406
    // MHz Distress Beacon Decoder (EPIR". Measured here so the box that
    // contains it is the height the name actually takes.
    const char* plateName = m.name.empty() ? "(unnamed module)" : m.name.c_str();
    const float nameH = wrapH(lf, uiPx, inner, plateName);

    const std::vector<PlateFact> facts = collectFacts(m);
    const std::vector<ReachRow> reach = collectReach(m);
    // WHICH KIND OF "NOT KNOWN" THIS IS. Chosen once, so the pass that
    // measures the box and the pass that letters it cannot pick differently.
    const char* unknownReach = (m.fitted && !m.loaded) ? kReachRefused : kReachUnknown;
    const float colW = (inner - cascade::gui::px(14.0f)) * 0.5f;

    // --- box 1: identity ----------------------------------------------------
    // THE LINE UNDER THE NAME IS THE SAME CLAIM AS THE CELLS BELOW IT, and it
    // used to make it twice as loudly: "maker not stated  ·  v?" for a file
    // whose descriptor was never read reports the maker's silence and a
    // missing version number, when the truth is that nobody has opened it.
    char meta[256];
    if (!m.haveDescriptor) {
        std::snprintf(meta, sizeof meta, "nothing was read out of this file");
    } else {
        std::snprintf(meta, sizeof meta, "%s  \xc2\xb7  v%s",
                      m.maker.empty() ? "maker not stated" : m.maker.c_str(),
                      m.version.empty() ? "?" : m.version.c_str());
    }
    const float blurbH = m.blurb.empty() ? 0.0f : (wrapH(uf, tiny, inner, m.blurb.c_str()) + 8.0f);
    const int factRows = (static_cast<int>(facts.size()) + 1) / 2;
    // THE ROW IS AS TALL AS THE TALLEST VALUE IN IT, measured in the face that
    // value will actually be drawn in. Every cell is drawn WRAPPED to its
    // column, so a value that no longer fits on one line does not clip - it
    // takes a second and prints through the key of the row beneath it. The two
    // that get close are "fitted, takes no signal" and "12, this build needs
    // 13", and whether either fits depends on the face's size AND on how
    // narrow the caller made the plate, which is exactly the pair of things a
    // constant cannot know.
    float factValueH = tinyH;
    for (const PlateFact& f : facts) {
        factValueH = std::max(
            factValueH, wrapH(faceForValue(f.value.c_str()), tiny, colW, f.value.c_str()));
    }
    const float factRowH = legH + 2.0f + factValueH + 8.0f;
    float box1H = kBoxPad + nameH + 3.0f + tinyH + blurbH + 10.0f + 1.0f + 10.0f +
                  factRowH * static_cast<float>(factRows) + kBoxPad - 8.0f;
    const float homeH = m.homepage.empty() ? 0.0f : (tinyH + 6.0f);
    box1H += homeH;

    // --- box 2: what this module reaches -------------------------------------
    const float markW = cascade::gui::px(18.0f);
    float box2H = kBoxPad + legH + 8.0f + wrapH(uf, tiny, inner, kReachLead) + 10.0f;
    if (reach.empty()) {
        box2H += noteHeight(inner, unknownReach);
    } else {
        for (const ReachRow& r : reach) {
            box2H += faceH(uf, tiny) + 2.0f +
                     wrapH(uf, tiny, inner - markW, r.detail.c_str()) + 8.0f;
        }
        box2H += 3.0f;
        box2H += noteHeight(inner, (m.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u
                                       ? kReachTuneNote
                                       : kReachNoTuneNote);
    }
    box2H += kBoxPad;

    // --- box 3: the maker's legal notice, only when there is one -------------
    float box3H = 0.0f;
    if (!m.legalNotice.empty()) {
        box3H = kBoxPad + legH + 8.0f + noteHeight(inner, m.legalNotice.c_str()) + kBoxPad;
    }

    // --- box 4: the refusal reason, only when the module was refused ---------
    float box4H = 0.0f;
    if (m.fitted && !m.loaded && !m.refusalReason.empty()) {
        box4H = kBoxPad + legH + 8.0f + noteHeight(inner, m.refusalReason.c_str()) + kBoxPad;
    }

    float total = box1H + kBoxGap + box2H;
    if (box3H > 0.0f) { total += kBoxGap + box3H; }
    if (box4H > 0.0f) { total += kBoxGap + box4H; }
    if (!draw || dl == nullptr) { return total; }

    // ======================= drawing ========================================
    float boxTop = tl.y;

    // ---- identity ----------------------------------------------------------
    {
        const ImVec2 bTL(tl.x, boxTop);
        const ImVec2 bBR(tl.x + width, boxTop + box1H);
        addPlateBox(dl, bTL, bBR);
        const float x = bTL.x + kBoxPad;
        float y = bTL.y + kBoxPad;
        dl->AddText(lf, uiPx, ImVec2(x, y), theme::kIvory, plateName, nullptr, inner);
        y += nameH + 3.0f;
        dl->AddText(uf, tiny, ImVec2(x, y), theme::kInkMuted, meta);
        y += tinyH;
        if (!m.blurb.empty()) {
            y += 8.0f;
            dl->AddText(uf, tiny, ImVec2(x, y), theme::kCream, m.blurb.c_str(), nullptr,
                        inner);
            y += blurbH - 8.0f;
        }
        y += 10.0f;
        addBenchRail(dl, x, bBR.x - kBoxPad, y);
        y += 10.0f;

        for (std::size_t i = 0; i < facts.size(); ++i) {
            const PlateFact& f = facts[i];
            // px() ON THE COLUMN GAP: colW's own reservation above already
            // subtracts px(14.0f) for it, and a raw 14.0f here quietly
            // stopped scaling with everything beside it once `width` (and so
            // `inner` and `colW`) started arriving in real device pixels -
            // the same fault fabc498 fixed on the SHOW well's second column.
            const float cx = x + (i % 2u == 0u ? 0.0f : (colW + cascade::gui::px(14.0f)));
            const float cy = y + factRowH * static_cast<float>(i / 2u);
            // THE KEY IS WHAT MAKES THE VALUE MEAN ANYTHING, so it is lettered
            // to be read: muted ink on the plate's dark ground is about 6:1
            // where the faint it used to take is about 4:1, and the value
            // under it still carries the emphasis in its own tone.
            dl->AddText(lf, tiny, ImVec2(cx, cy), theme::kInkMuted, f.key);
            const ImVec2 vAt(cx, cy + legH + 2.0f);
            if (f.hatched) {
                addHatch(dl, ImVec2(vAt.x, vAt.y + 1.0f),
                         ImVec2(vAt.x + colW, vAt.y + tinyH - 1.0f));
            }
            ImFont* vf = faceForValue(f.value.c_str());
            dl->AddText(vf, tiny, vAt, f.hatched ? theme::kInkFaint : f.tone,
                        f.value.c_str(), nullptr, colW);
        }
        y += factRowH * static_cast<float>(factRows);
        if (!m.homepage.empty()) {
            // A URL IS FOR COPYING, so it is lettered to be transcribed rather
            // than to be sensed. Faint ink on this ground is about 4:1, which
            // is where a run of punctuation stops being readable first.
            dl->AddText(uf, tiny, ImVec2(x, y - 2.0f), theme::kInkMuted, m.homepage.c_str(),
                        nullptr, inner);
        }
        boxTop = bBR.y + kBoxGap;
    }

    // ---- what this module reaches ------------------------------------------
    {
        const ImVec2 bTL(tl.x, boxTop);
        const ImVec2 bBR(tl.x + width, boxTop + box2H);
        addPlateBox(dl, bTL, bBR);
        const float x = bTL.x + kBoxPad;
        float y = bTL.y + kBoxPad;
        addBenchGroupCaption(dl, ImVec2(x, y), inner, "WHAT THIS MODULE REACHES");
        y += legH + 8.0f;
        dl->AddText(uf, tiny, ImVec2(x, y), theme::kInkMuted, kReachLead, nullptr, inner);
        y += wrapH(uf, tiny, inner, kReachLead) + 10.0f;

        if (reach.empty()) {
            drawNote(dl, ImVec2(x, y), inner, theme::kGold, unknownReach);
        } else {
            for (const ReachRow& r : reach) {
                // The mark: a filled, glowing dot for a capability that
                // reaches outward, a hollow ring for one that only produces
                // output. Never rust - a declared capability is not a fault,
                // and rust in this palette means trouble.
                const ImVec2 c(x + 5.0f, y + faceH(uf, tiny) * 0.5f);
                if (r.outward) {
                    dl->AddCircleFilled(c, 4.5f, theme::withAlpha(theme::kGold, 0.28f), 12);
                    dl->AddCircleFilled(c, 3.0f, theme::kGold, 12);
                } else {
                    dl->AddCircle(c, 3.5f, theme::kInkFaint, 12, 1.5f);
                }
                dl->AddText(uf, tiny, ImVec2(x + markW, y),
                            r.outward ? theme::kIvory : theme::kCream, r.key);
                y += faceH(uf, tiny) + 2.0f;
                // THE SENTENCE UNDER EACH REACH ROW IS THE ANSWER to what the
                // module can actually do with this machine - the one thing on
                // this plate a user reads before deciding to fit something.
                // Muted rather than faint for that reason alone.
                dl->AddText(uf, tiny, ImVec2(x + markW, y), theme::kInkMuted,
                            r.detail.c_str(), nullptr, inner - markW);
                y += wrapH(uf, tiny, inner - markW, r.detail.c_str()) + 8.0f;
            }
            y += 3.0f;
            drawNote(dl, ImVec2(x, y), inner, theme::kGold,
                     (m.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u ? kReachTuneNote
                                                                      : kReachNoTuneNote);
        }
        boxTop = bBR.y + kBoxGap;
    }

    // ---- the maker's legal notice ------------------------------------------
    if (box3H > 0.0f) {
        const ImVec2 bTL(tl.x, boxTop);
        const ImVec2 bBR(tl.x + width, boxTop + box3H);
        addPlateBox(dl, bTL, bBR);
        const float x = bTL.x + kBoxPad;
        float y = bTL.y + kBoxPad;
        addBenchGroupCaption(dl, ImVec2(x, y), inner, "LEGAL NOTICE");
        y += legH + 8.0f;
        // VERBATIM. Some decoders demodulate transmissions whose interception
        // is an offence in some countries; this is the author saying so, and
        // paraphrasing it would be answering for them.
        drawNote(dl, ImVec2(x, y), inner, theme::kGold, m.legalNotice.c_str());
        boxTop = bBR.y + kBoxGap;
    }

    // ---- why a fitted module was refused ------------------------------------
    if (box4H > 0.0f) {
        const ImVec2 bTL(tl.x, boxTop);
        const ImVec2 bBR(tl.x + width, boxTop + box4H);
        addPlateBox(dl, bTL, bBR);
        const float x = bTL.x + kBoxPad;
        float y = bTL.y + kBoxPad;
        addBenchGroupCaption(dl, ImVec2(x, y), inner, "WHY IT IS NOT RUNNING");
        y += legH + 8.0f;
        // PluginHost's own reason, word for word. "My plugin does not appear"
        // with no explanation is the support ticket the host was written to
        // prevent, and re-wording its answer here would put it back.
        drawNote(dl, ImVec2(x, y), inner, theme::kAlarm, m.refusalReason.c_str());
    }
    return total;
}

// --- filtering and ordering ----------------------------------------------------

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

bool matchesQuery(const StoreModule& sm, const std::string& lowerQuery) {
    if (lowerQuery.empty()) { return true; }
    const std::string hay =
        lowerAscii(sm.plate.name + " " + sm.plate.maker + " " + sm.plate.blurb);
    return hay.find(lowerQuery) != std::string::npos;
}

// ---------------------------------------------------------------------------
// HAS ANYBODY ASKED, AND WHAT CAME BACK
// ---------------------------------------------------------------------------
//
// FOUR ANSWERS, NOT TWO. "There are no rows" was drawn as "no catalogue has
// been read" everywhere in this window, which is right for a store nobody has
// pressed CHECK NOW on and wrong - and unescapable - for the two other ways to
// have no rows: a fetch that succeeded and returned an index listing no
// modules, and a fetch that failed. Both of those have been asked, and telling
// their user to press CHECK NOW is telling them to do again the thing they
// just did.
//
// The evidence is the pair of strings AppWindow clears at the start of every
// fetch and fills in at the end of it; see PluginStoreModel for why status is
// tested before error.
// FIVE ANSWERS NOW, and the fifth is not a way of having no rows - it is a
// build that will never have a catalogue at all. See PluginStoreModel::bundled.
enum class CatalogueState {
    NeverAsked,  // nothing fetched this session
    Failed,      // asked, and the attempt failed. sourceError says why
    ReadEmpty,   // asked, answered, and the index listed no modules
    Read,        // asked, answered, and there are rows
    Bundled,     // there is no catalogue: the modules are inside this build
};

CatalogueState catalogueState(const PluginStoreModel& m) {
    // TESTED FIRST, AND IT OVERRIDES EVERY OTHER ANSWER INCLUDING Read. The
    // rows in a bundled build are the loaded modules, not a fetched index, so
    // reporting "the catalogue was read" would be a claim about a fetch that
    // never happened - and it is the branch that decides whether this window
    // tells the user to press a key that cannot work.
    if (m.bundled) { return CatalogueState::Bundled; }
    if (m.haveCatalogue) { return CatalogueState::Read; }
    if (!m.sourceStatus.empty()) { return CatalogueState::ReadEmpty; }
    if (!m.sourceError.empty()) { return CatalogueState::Failed; }
    return CatalogueState::NeverAsked;
}

// The three disjoint, exhaustive state categories the SHOW well switches on.
enum class StateGroup { Fitted, Available, Blocked };

StateGroup stateGroup(const StoreModule& sm) {
    if (sm.plate.fitted) { return StateGroup::Fitted; }
    return sm.installableHere ? StateGroup::Available : StateGroup::Blocked;
}

// ...and the three disjoint, exhaustive kind categories.
enum class KindGroup { Decoder, Other, Undeclared };

KindGroup kindGroup(const ModulePlate& m) {
    if (!m.haveCapabilities) { return KindGroup::Undeclared; }
    constexpr std::uint32_t kDecoderBits =
        CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;
    return (m.capabilities & kDecoderBits) != 0u ? KindGroup::Decoder : KindGroup::Other;
}

bool passesShow(const StoreModule& sm, const PluginStoreDeck& d) {
    switch (stateGroup(sm)) {
        case StateGroup::Fitted:
            if (!d.showFitted) { return false; }
            break;
        case StateGroup::Available:
            if (!d.showAvailable) { return false; }
            break;
        case StateGroup::Blocked:
            if (!d.showBlocked) { return false; }
            break;
    }
    switch (kindGroup(sm.plate)) {
        case KindGroup::Decoder: return d.showDecoders;
        case KindGroup::Other: return d.showOtherKinds;
        case KindGroup::Undeclared: return d.showUndeclared;
    }
    return true;
}

}  // namespace

// --- the shared plate, and the row vocabulary that goes with it ---------------

float moduleDataPlateHeight(float width, const ModulePlate& m) {
    return layoutPlate(nullptr, ImVec2(0.0f, 0.0f), width, m, false);
}

float drawModuleDataPlate(ImDrawList* dl, const ImVec2& tl, float width,
                          const ModulePlate& m) {
    return layoutPlate(dl, tl, width, m, true);
}

const char* moduleKindTag(const ModulePlate& m) {
    if (!m.haveCapabilities) {
        // TWO REASONS, TWO TAGS. A catalogue row has not declared anything to
        // us YET, and fitting it is what fills that in; a file the host would
        // not accept has no kind on this panel at all, and tagging it "NOT
        // DECLARED" would put the silence on the module rather than on the
        // refusal.
        return (m.fitted && !m.loaded) ? "NOT KNOWN" : "NOT DECLARED";
    }
    const std::uint32_t c = m.capabilities;
    if ((c & (CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER)) !=
        0u) {
        return "DECODER";
    }
    if ((c & (CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) !=
        0u) {
        return "MAP";
    }
    if ((c & (CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT)) != 0u) { return "PANEL"; }
    if ((c & (CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_PRESET)) != 0u) { return "CONTROL"; }
    return "MODULE";
}

// See plugin_store_view.hpp. Every word moduleKindTag can return, measured in
// the face the chip is lettered in, plus the shoulder the chip needs either
// side of it. The list is written out rather than derived, because a tag
// missing from it is a chip that overflows in exactly the state nobody tests.
float storeCheckKeyWidth() {
    // THE THREE WORDS THIS ONE KEY CAN CARRY, and the third is why this is a
    // function. "NO CATALOGUE" is what it is engraved with in a build whose
    // modules are bundled (PluginStoreModel::bundled), and it is LONGER than
    // either CHECK word - so a max that lists only those two leaves the key at
    // the old width and drawDeckKey, which centres its label and neither wraps
    // nor clips, hangs the longer word out over both machined edges.
    //
    // It was a local inside draw(), where nothing but a screenshot at one
    // scale could observe it. Exported for exactly the reason
    // moduleKindTagWidth() is: tests/test_bench_text_fits.cpp can then hold it
    // to the widest string it may be asked to hold, at any UI scale.
    static const char* const kWords[] = {"CHECK NOW", "CHECK AGAIN", "NO CATALOGUE"};
    ImFont* f = fonts::ui();
    const float px = prose();
    float w = 0.0f;
    for (const char* s : kWords) { w = std::max(w, textW(f, px, s)); }
    // The floor is the design's own key width, so a narrow face cannot shrink
    // the key out of the deck; the shoulder is the one every other key here
    // uses.
    return std::max(cascade::gui::px(92.0f), w + cascade::gui::px(22.0f));
}

float moduleKindTagWidth() {
    static const char* const kTags[] = {"NOT KNOWN", "NOT DECLARED", "DECODER",
                                        "MAP",       "PANEL",        "CONTROL",
                                        "MODULE"};
    ImFont* f = fonts::ui();
    const float px = cascade::gui::px(fonts::kTinySize);
    float w = 0.0f;
    for (const char* t : kTags) { w = std::max(w, textW(f, px, t)); }
    // The floor is the width the store's card used before this was measured,
    // so a narrow face cannot shrink the chip out of the design.
    return std::max(cascade::gui::px(84.0f), w + cascade::gui::px(14.0f));
}

// THE MODULE CARD'S OWN ACTION-COLUMN WIDTH, exported for the same reason as
// the two functions above it: the module list's row and moduleRowColumns()
// below both need the identical figure, and a second copy of this max() list
// is exactly the kind of drift this file measures rather than guesses at.
float moduleActionColumnWidth() {
    ImFont* uf = fonts::ui();
    const float tiny = prose();
    return std::max({cascade::gui::px(150.0f), textW(uf, tiny, "FIT") + cascade::gui::px(28.0f),
                     textW(uf, tiny, "UPDATE") + cascade::gui::px(28.0f),
                     textW(uf, tiny, "FITTED") + cascade::gui::px(28.0f),
                     textW(uf, tiny, "NOT INSTALLED") + cascade::gui::px(18.0f),
                     textW(uf, tiny, "CANNOT FIT") + cascade::gui::px(18.0f),
                     textW(uf, tiny, "INSTALLED") + cascade::gui::px(18.0f),
                     textW(uf, tiny, "REFUSED") + cascade::gui::px(18.0f)});
}

// HOW ONE MODULE CARD'S THREE COLUMNS DIVIDE `cw` - the card's own width,
// which is the module list's content width and is NOT the desktop's 1280 px
// store this row was first drawn for. A pure function of `cw` (font metrics
// come from the loaded typefaces, not from an open ImGui frame or a window
// size), so tests/test_plugin_store_deck.cpp can pin `mx + midW == ax` - the
// text column ends EXACTLY where the action column begins, never later, at
// any width - rather than trusting a screenshot to notice when it does not.
//
// midW IS NEVER A COMFORT FLOOR THAT CAN EXCEED THE ROOM ACTUALLY LEFT. It
// used to be std::max(px(120.0f), cw - kTagW - kActW - kCardPad * 3.0f): a
// floor meant for a card so narrow the text column would otherwise collapse
// to nothing. At UI scale 2.0 that floor is itself scaled (px(120.0f) is
// 240 device px, not 120), so on a card even a little narrower than the
// desktop's own the CLAIMED 240 px could exceed the true remainder by a
// wide margin - and on the real docked tablet body (1174 px, module list
// content width ~712-727 px) it did: the floor claimed more than was left,
// and nothing clipped the difference, which is exactly what drew
// "INSTALLED" and "STARTED" on top of the row's own text. A card this
// narrow gets an honestly narrow text column now instead - see the
// per-glyph clip on the row's version/install and maker/licence lines, the
// same technique railPlateLabel and centreDockTabLabel already use for the
// same reason: arbitrary third-party text with nothing else bounding it.
// (The data plate's own width - `plateW`, further down this file - has an
// unrelated px() shortfall of its own that this change does not touch; see
// the note there for why.)
ModuleRowColumns moduleRowColumns(float cw) {
    ModuleRowColumns r;
    const float kTagW = moduleKindTagWidth();
    const float kActW = moduleActionColumnWidth();
    const float kCardPad = cascade::gui::px(14.0f);
    r.mx = kCardPad + kTagW + kCardPad;
    r.ax = cw - kCardPad - kActW;
    r.midW = std::max(0.0f, r.ax - r.mx);
    return r;
}

std::string moduleReachSummary(const ModulePlate& m) {
    // NEVER "reaches nothing". Every plugin here is native code mapped into
    // this process; there is no data-only module type, so no module reaches
    // nothing and no row may say it does.
    if (!m.haveCapabilities) {
        // A refused file IS fitted, so "until it is fitted" would be false of
        // it - and it is silent because the host would not have it, not
        // because it asks for nothing.
        return (m.fitted && !m.loaded) ? "not known: the host did not accept this file"
                                       : "not declared until it is fitted";
    }
    if ((m.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u) {
        return m.haveTuneGrant && m.tuneGranted ? "granted: may move the receiver"
                                                : "asks to move the receiver";
    }
    if ((m.capabilities & (CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) != 0u) {
        // "a server you choose" was false of both: neither capability takes a
        // server from this console, so whatever they reach is the module's
        // choice and the console never learns what it was. "may" because
        // nothing here can tell whether the source is on the network at all -
        // the reach list below says the same thing at length.
        return "may fetch from a server it chose";
    }
    return "publishes to the host only";
}

ImU32 moduleReachColour(const ModulePlate& m) {
    // FAINT STAYS FAINT HERE, and it was tried the other way. Raising the
    // unknown case to kInkMuted for legibility would have made it the SAME
    // tone as the inward case two lines below - two different answers in one
    // colour, which is worse than a dim one, and testReachColour rejected it
    // on exactly that ground. The three tones on this ladder are the whole
    // signal; the legibility of the sentence they colour is bought with the
    // size raise instead.
    if (!m.haveCapabilities) { return theme::kInkFaint; }
    if ((m.capabilities &
         (CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) != 0u) {
        return theme::kGold;
    }
    return theme::kInkMuted;
}

const char* moduleStateWord(const ModulePlate& m) {
    switch (plateState(m)) {
        case PlateState::NotFitted: return "NOT FITTED";
        case PlateState::Refused: return "REFUSED";
        case PlateState::Stopped: return "STOPPED";
        case PlateState::NoSignal: return "TAKES NO SIGNAL";
        case PlateState::Started: return "STARTED";
    }
    return "NOT FITTED";
}

ImU32 moduleStateColour(const ModulePlate& m) {
    switch (plateState(m)) {
        // FAINT STAYS FAINT HERE, for the same reason as moduleReachColour
        // above: NoSignal three lines down is kInkMuted, so lifting NotFitted
        // to it would letter two of the five states identically. Five states
        // need five tones more than one of them needs a brighter one, and
        // testStateInkAndLamp rejected the change on that ground.
        case PlateState::NotFitted: return theme::kInkFaint;
        case PlateState::Refused: return theme::kAlarm;
        // A stop is a choice the user made, so it letters in plain ink rather
        // than in anything that reads as trouble - the same rule, and the same
        // tone, the FITTED MODULES window uses for it.
        case PlateState::Stopped: return theme::kCream;
        case PlateState::NoSignal: return theme::kInkMuted;
        // NOT PHOSPHOR. Phosphor in this palette means something is working,
        // and "started" is not "working" - see the header.
        case PlateState::Started: return theme::kIvory;
    }
    return theme::kInkFaint;
}

bool moduleStateLampLit(const ModulePlate& m) {
    return plateState(m) == PlateState::Refused;
}

const char* storeSortLabel(int index) {
    switch (index) {
        case 1: return "MAKER";
        case 2: return "VERSION";
        default: return "NAME";
    }
}

// See the header, and fonts.hpp for why kPanelSize exists at all. It is the
// theme's own largest size, added for this page and read by nothing else, so
// the raise cannot move the rail, the spectrum axis or a meter face - the
// exact sweep raising kUiSize cost in 0.79.0 and gave back in 0.84.0. Nothing
// here invents a figure.
float storeProsePx() { return fonts::kPanelSize; }

// ===========================================================================
// THE INSTALL STATE - the catalogue's question, not the runner's
// ===========================================================================

StoreInstallState storeInstallState(const StoreModule& sm) {
    const ModulePlate& p = sm.plate;
    if (p.fitted) {
        // REFUSED FIRST. A file that is here and that the host would not have
        // is the most important thing this window can say about it, and it is
        // the same word - and the same ink - moduleStateWord uses, so the row
        // and the plate beside it cannot describe one module two ways.
        if (!p.loaded) { return StoreInstallState::Refused; }
        if (!sm.updateToVersion.empty()) { return StoreInstallState::UpdateAvailable; }
        return StoreInstallState::Installed;
    }
    // NOT INSTALLED AND CANNOT FIT ARE DIFFERENT ANSWERS and the difference is
    // whether anything the user does could change it. installableHere is the
    // STABLE fact - an exact ABI match and a build for this os/arch - and
    // deliberately not blockedReason, which also carries "a transfer is
    // already in progress" and would move a row between two words while a
    // download ran.
    return sm.installableHere ? StoreInstallState::NotInstalled
                              : StoreInstallState::CannotFit;
}

const char* storeInstallWord(StoreInstallState s) {
    switch (s) {
        case StoreInstallState::NotInstalled: return "NOT INSTALLED";
        case StoreInstallState::CannotFit: return "CANNOT FIT";
        case StoreInstallState::Installed: return "INSTALLED";
        case StoreInstallState::UpdateAvailable: return "UPDATE";
        case StoreInstallState::Refused: return "REFUSED";
    }
    return "NOT INSTALLED";
}

ImU32 storeInstallColour(StoreInstallState s) {
    switch (s) {
        // PLAIN INK, NOT FAINT AND NOT GOLD. Most of the catalogue is in this
        // state on a fresh machine, so it is the word the user reads most
        // often - and not having something is not a fault to be coloured as
        // one.
        case StoreInstallState::NotInstalled: return theme::kCream;
        // A fact about this machine rather than a fault of the module. Muted,
        // which is a clear step below the cream above it; the gold note on the
        // row carries the reason at length.
        case StoreInstallState::CannotFit: return theme::kInkMuted;
        case StoreInstallState::Installed: return theme::kPhosphor;
        // GOLD, NOT AMBER. Amber in this palette is a READING - something the
        // radio or the machine measured - and an offer from a catalogue is
        // not a measurement. Gold is this window's "something to look at",
        // and it is what the updates banner above already letters in.
        case StoreInstallState::UpdateAvailable: return theme::kGold;
        case StoreInstallState::Refused: return theme::kAlarm;
    }
    return theme::kCream;
}

// ===========================================================================
// ADD ALL - what it picks, and what the key says
// ===========================================================================

AddAllPlan planAddAll(const PluginStoreModel& model, bool noticesAcknowledged) {
    AddAllPlan plan;

    for (int i = 0; i < static_cast<int>(model.modules.size()); ++i) {
        const StoreModule& sm = model.modules[static_cast<std::size_t>(i)];
        const std::string& name = sm.plate.name;
        const std::string shown = name.empty() ? std::string("(unnamed module)") : name;
        if (sm.plate.fitted) {
            // A FITTED MODULE IS ONLY EVER AN UPDATE HERE. It is never listed
            // as skipped: "already installed" is the outcome the user pressed
            // this key for, not a thing that went wrong, and a summary that
            // reported five of them as passed over would bury the one that
            // actually could not be fitted.
            if (!sm.updateToVersion.empty()) { plan.update.push_back(i); }
            continue;
        }
        // THE SAME GATE A SINGLE FIT GOES THROUGH, asked of every row - with
        // the notice treated as acknowledged only when the user has ticked
        // the one box beside this key.
        const std::string& why =
            noticesAcknowledged ? sm.blockedReasonIfAcknowledged : sm.blockedReason;
        if (why.empty()) {
            plan.install.push_back(i);
            continue;
        }
        // HELD BY A NOTICE AND NOTHING ELSE is the one skip the user can undo
        // from this panel, so it is counted apart from the rest.
        if (!sm.plate.legalNotice.empty() && sm.blockedReasonIfAcknowledged.empty()) {
            ++plan.heldByNotice;
        }
        plan.skipped.push_back(shown + " - " + why);
    }

    const int n = static_cast<int>(plan.install.size());
    const int m = static_cast<int>(plan.update.size());
    char buf[96];
    if (n > 0 && m > 0) {
        std::snprintf(buf, sizeof buf, "ADD %d PLUGIN%s, UPDATE %d", n, n == 1 ? "" : "S",
                      m);
        plan.label = buf;
    } else if (n > 0) {
        // "ALL" ONLY WHEN IT REALLY IS ALL. A key engraved ADD ALL PLUGINS
        // that quietly passes over seven of them is the kind of copy this
        // window exists to refuse.
        if (plan.skipped.empty()) {
            plan.label = "ADD ALL PLUGINS";
        } else {
            std::snprintf(buf, sizeof buf, "ADD %d PLUGIN%s", n, n == 1 ? "" : "S");
            plan.label = buf;
        }
    } else if (m > 0) {
        std::snprintf(buf, sizeof buf, "UPDATE %d PLUGIN%s", m, m == 1 ? "" : "S");
        plan.label = buf;
    } else {
        plan.label = "ADD ALL PLUGINS";
    }

    // --- and why it may not be pressed --------------------------------------
    //
    // THE SAME FIVE CATALOGUE STATES the rest of the window distinguishes:
    // nobody has asked, it was asked and failed, it was asked and listed
    // nothing, it was read, or this build has no catalogue at all. Telling a
    // user whose check just failed to press CHECK NOW is telling them to do
    // again the thing that did not work; telling a user with a bundled build
    // to press it is worse, because there is no key.
    if (model.bundled) {
        // BEFORE the haveCatalogue test, because a bundled build DOES have
        // rows - they are the modules loaded out of the apk - and the reason
        // ADD ALL is dead here is not "there is nothing to list", it is "there
        // is nowhere to add from".
        plan.blockedReason =
            "every module in this build is already fitted: they are compiled into it "
            "and there is nothing to fetch";
    } else if (!model.haveCatalogue) {
        if (!model.sourceStatus.empty()) {
            plan.blockedReason = "the catalogue was read and it lists no modules at all";
        } else if (!model.sourceError.empty()) {
            plan.blockedReason =
                "the last check did not return a catalogue - its reason is under "
                "CATALOGUE SOURCE";
        } else {
            plan.blockedReason =
                "no catalogue has been read yet - press CHECK NOW and this application "
                "asks the source once";
        }
    } else if (model.busy) {
        // One transfer at a time is what the downloader actually does, so a
        // second run started over the first would be two operations sharing
        // one progress bar and one CANCEL.
        plan.blockedReason = "a transfer is already in progress";
    } else if (n == 0 && m == 0) {
        plan.blockedReason =
            plan.skipped.empty()
                ? "every module in the catalogue is already fitted, and none has a "
                  "newer build"
                : "nothing in the catalogue can be fitted on this machine - each "
                  "module's own reason is on its row";
    }
    return plan;
}

// ===========================================================================
// THE WINDOW
// ===========================================================================

void PluginStoreView::draw(float width, float height, const PluginStoreModel& model,
                           PluginStoreDeck& deck) {
    // Cleared first, so a request is answered once or not at all.
    checkNow_ = false;
    cancel_ = false;
    addAll_ = false;
    fitIndex_ = -1;
    updateIndex_ = -1;
    // Zeroed rather than left stale: the "too narrow" bail-out below returns
    // before the deck is measured at all, and 0 says so honestly where the
    // previous frame's figure would claim a deck that was never laid out.
    upperDeckHeight_ = 0.0f;
    addAllWellDrawn_ = false;

    ImGui::PushID("pluginstore");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // TOO NARROW TO DRAW HONESTLY, so it says so instead of drawing a squashed
    // deck with controls collapsed to nothing. This is a real operating-system
    // window the user can drag to any size, and a panel that silently omits
    // half its switches at 400px is worse than one that asks to be widened.
    // 640, NOT 560: the deck is three wells side by side and every word in
    // them grew with storeProsePx(), so the figure the old face fitted in is
    // no longer the figure this one does. Measured the same way it always was
    // - by widening the window until the rockers' label plates stop running
    // into their counts.
    if (width < 640.0f || height < 260.0f) {
        const char* small =
            "This window is too narrow to lay out the catalogue. Widen it and the deck, "
            "the module list and the data plate come back.";
        if (width > 80.0f) {
            drawNote(dl, origin, std::max(60.0f, width - 8.0f), theme::kGold, small);
        }
        ImGui::Dummy(ImVec2(std::max(1.0f, width), std::max(1.0f, height)));
        ImGui::PopID();
        return;
    }

    ImFont* uf = fonts::ui();
    ImFont* lf = fonts::legend();
    ImFont* rf = fonts::reading();
    const float tiny = prose();
    // THE NAME LINE TAKES THE SAME LARGEST SIZE as the prose under it and is
    // told apart by its FACE - Georgia Bold against Georgia Regular - rather
    // than by a second figure. One size for the page is what makes "go bigger
    // on the font" one edit; a heading size on top of it would be a second
    // number to keep in step with the first.
    const float uiPx = prose();
    const float tinyH = faceH(uf, tiny);
    const float legH = faceH(lf, tiny);
    const float nameH = faceH(lf, uiPx);

    const float kPad = cascade::gui::px(10.0f);
    const float kGap = cascade::gui::px(10.0f);
    // THE CONTROL HEIGHTS, MEASURED RATHER THAN TYPED. Each was fitted around
    // a 12 px engraving and each holds a different amount of it, so no single
    // adjustment would have been right for all three:
    //
    //   A KEY carries one centred word.
    //
    //   A SEGMENT carries one centred word in the shallowest of the three.
    //
    // And the PLATE KEY at the foot of the data plate carries TWO lines -
    // "UPDATE MODULE" over "TO v1.2.3" - so it is the one that runs out of
    // room first as the face grows: 34 px holds two 14 px lines with three
    // pixels top and bottom, and two 17 px lines not at all.
    //
    // The old figures stay as floors: they are the design's proportions and
    // nothing here should shrink if a future face happens to be short.
    //
    // THE THIRD OF THE THREE - A ROCKER, which carries a label PLATE the
    // face's height plus five, rows stacked with no gap so a row only as tall
    // as its own plate makes two neighbouring plates touch - is computed in
    // THE CONTROL DECK below instead of here, because on the narrowest real
    // bodies this window has been measured on its label reads a size down
    // (see `rockerH` and the note on `compact`), which this file's OTHER two
    // heights never need to.
    const float kKeyH = std::max(cascade::gui::px(28.0f), tinyH + cascade::gui::px(12.0f));
    const float kSegH = std::max(cascade::gui::px(24.0f), tinyH + cascade::gui::px(10.0f));
    const float kPlateKeyH =
        std::max(cascade::gui::px(34.0f), tinyH * 2.0f + cascade::gui::px(8.0f));
    // AND THE FIXED KEY WIDTHS, each from the widest word it can carry. Every
    // one of these was a literal, and a key whose word no longer fits does not
    // wrap or clip - drawDeckKey CENTRES its label, so the word simply hangs
    // out over both machined edges.
    const float kClearW =
        std::max(cascade::gui::px(60.0f), textW(uf, tiny, "CLEAR") + cascade::gui::px(22.0f));
    // EXPORTED, LIKE THE KIND TAG'S WIDTH, because its widest word is now one
    // no test could otherwise see - see storeCheckKeyWidth().
    const float kCheckW = storeCheckKeyWidth();
    const float kUpdKeyW =
        std::max(cascade::gui::px(96.0f), textW(uf, tiny, "UPDATE") + cascade::gui::px(22.0f));
    // The banner's caption column: a lamp, then the longest of the five
    // headings it can show, then the "n MODULES" line under it. Sized for the
    // widest so the divider and the note beside it do not move when the
    // catalogue's state changes.
    const float kBannerCapW =
        std::max({cascade::gui::px(178.0f),
                  cascade::gui::px(6.0f) * 2.0f + cascade::gui::px(8.0f) +
                      std::max({textW(lf, tiny, "CATALOGUE NOT READ"),
                                textW(lf, tiny, "LAST CHECK FAILED"),
                                textW(lf, tiny, "CATALOGUE IS EMPTY"),
                                textW(lf, tiny, "UPDATES AVAILABLE"),
                                textW(lf, tiny, "NO UPDATES")}) +
                      kPad * 2.0f});

    // --- which rows are on screen, and in what order -------------------------
    const std::string q = lowerAscii(std::string(deck.search));
    std::vector<int> visible;
    visible.reserve(model.modules.size());
    int hiddenByShow = 0;
    for (int i = 0; i < static_cast<int>(model.modules.size()); ++i) {
        const StoreModule& sm = model.modules[static_cast<std::size_t>(i)];
        if (!passesShow(sm, deck)) {
            ++hiddenByShow;
            continue;
        }
        if (!matchesQuery(sm, q)) { continue; }
        visible.push_back(i);
    }
    const int sortKey = std::clamp(deck.sortKey, 0, kStoreSortCount - 1);
    deck.sortKey = sortKey;
    std::sort(visible.begin(), visible.end(), [&](int a, int b) {
        const ModulePlate& pa = model.modules[static_cast<std::size_t>(a)].plate;
        const ModulePlate& pb = model.modules[static_cast<std::size_t>(b)].plate;
        if (sortKey == 1) {
            const std::string la = lowerAscii(pa.maker);
            const std::string lb = lowerAscii(pb.maker);
            if (la != lb) { return la < lb; }
        } else if (sortKey == 2) {
            // The PRODUCT'S comparator, not a string compare. It orders dotted
            // parts as NUMBERS, which is the difference between putting 1.10.0
            // above 1.9.0 and below it - and reusing it is what stops this list
            // and the update planner disagreeing about which build is newer.
            const int c = cascade::core::PluginRepo::compareVersions(pa.version, pb.version);
            if (c != 0) { return c > 0; }
        }
        return lowerAscii(pa.name) < lowerAscii(pb.name);
    });

    // The selection follows the catalogue rather than an index that may now
    // name a different module. Out of range picks the first visible row, and
    // any move clears the legal acknowledgement - a tick given to one plugin
    // is consent for that plugin and nothing else.
    const int wanted = deck.selected;
    if (deck.selected < 0 || deck.selected >= static_cast<int>(model.modules.size()) ||
        std::find(visible.begin(), visible.end(), deck.selected) == visible.end()) {
        deck.selected = visible.empty() ? -1 : visible.front();
    }
    if (deck.selected != wanted) { deck.legalAck = false; }

    int updateCount = 0;
    for (const StoreModule& sm : model.modules) {
        if (!sm.updateToVersion.empty()) { ++updateCount; }
    }

    // MOVED AHEAD OF THE ADD ALL WELL, which now needs to know this before it
    // decides whether to exist at all - see the note on CatalogueState::Bundled
    // just below. THE UPDATES BANNER (further down) uses the same value it
    // always did; this is not a second computation of it, just an earlier one.
    const CatalogueState catState = catalogueState(model);

    // ======================= ADD ALL PLUGINS ================================
    //
    // THE ONE KEY AT THE TOP OF THE PAGE, and it is the largest thing on it
    // because it is the only control here that acts on the whole catalogue.
    // Everything it does, a single FIT already did: the same download, the
    // same https rule, the same sha256, the same ABI test, the same refusal
    // messages - one after another, because PluginRepo applies exactly one
    // transfer at a time and this key does not get to be the exception.
    //
    // WHAT IT REFUSES TO DO is take a consent nobody gave. Seven of the
    // twenty-four modules in the live catalogue carry a maker's legal notice,
    // and a key that swept those in silently would be the worst kind of bulk
    // action there is. They are named, they are counted, and one tick beside
    // the key adds them - or does not, and the key says ADD 17 PLUGINS
    // instead of ADD ALL PLUGINS, which is the truth about what it will do.
    //
    // NOT DRAWN AT ALL IN A BUNDLED BUILD, and this is a harder rule than
    // "disabled". Every OTHER dead state here still draws the key, greyed,
    // with its reason beside it - a live control that cannot be pressed today
    // is still the truthful shape of the window. Bundled is different: there
    // is no catalogue on this platform AT ALL (see PluginStoreModel::bundled),
    // so this key can never do anything on any day the application looks like
    // this, on any device. A permanently dead key that claims a THIRD of the
    // page's own width and a fifth of a docked tablet's whole body is not
    // "the largest thing on the page because it acts on the whole catalogue"
    // any more - it is furniture with a caption taped to it. Its one sentence
    // (why it cannot run) folds into THE UPDATES BANNER below instead, which
    // already carries the Bundled state's own explanation and loses nothing
    // by carrying this one too. Every other catalogue state keeps the well
    // exactly as it always drew - this is the one branch, and it is decided
    // by the same `catState` the banner already computes.
    float addAllH = 0.0f;
    float addAllTotal = 0.0f;
    if (catState != CatalogueState::Bundled) {
        const AddAllPlan plan = planAddAll(model, deck.addAllAck);
        int noticeModules = 0;
        std::string noticeNames;
        for (const StoreModule& sm : model.modules) {
            if (sm.plate.fitted || sm.plate.legalNotice.empty()) { continue; }
            if (!sm.blockedReasonIfAcknowledged.empty()) { continue; }
            ++noticeModules;
            if (!noticeNames.empty()) { noticeNames += ", "; }
            noticeNames += sm.plate.name.empty() ? "(unnamed module)" : sm.plate.name;
        }

        // The key is measured from the longest engraving it can ever carry, not
        // from the one it happens to have: drawDeckKey CENTRES its label and does
        // not clip, so a key too narrow does not shorten the word - it hangs it
        // out over both machined edges.
        const float addKeyW = std::max(
            {cascade::gui::px(320.0f), textW(uf, tiny, plan.label.c_str()) + cascade::gui::px(40.0f),
             textW(uf, tiny, "ADD ALL PLUGINS") + cascade::gui::px(40.0f)});
        const float addKeyH =
            std::max(cascade::gui::px(54.0f), tinyH * 2.0f + cascade::gui::px(18.0f));
        const float addNoteW = width - kPad * 3.0f - addKeyW - cascade::gui::px(12.0f);

        std::string addLead;
        ImU32 addAccent = theme::kInkMuted;
        if (model.addAllRunning) {
            addLead = model.addAllProgress.empty()
                          ? std::string("Working through the catalogue, one module at a time.")
                          : model.addAllProgress;
            addAccent = theme::kGold;
        } else if (!plan.blockedReason.empty()) {
            // A DEAD KEY ALWAYS SAYS WHY - the rule this whole window is built on.
            addLead = "Cannot add all: " + plan.blockedReason;
            addAccent = theme::kGold;
        } else {
            char lead[512];
            std::snprintf(lead, sizeof lead,
                          "%d to fetch and %d to update, one after another. Each is fetched "
                          "over https and refused unless its bytes hash to the sha256 the "
                          "catalogue published - the same gate a single FIT goes through. A "
                          "module that fails does not stop the rest.",
                          static_cast<int>(plan.install.size()),
                          static_cast<int>(plan.update.size()));
            addLead = lead;
        }

        std::string addSkipLine;
        if (!model.addAllRunning && noticeModules > 0) {
            // BUILT AS A STRING, NOT INTO A BUFFER. Seven module names run past
            // three hundred characters and a 320-byte snprintf cut the sentence at
            // "...is on that module's DAT" - a truncated sentence about consent,
            // on the one note whose job is to say exactly what is being consented
            // to. There is no length that is safely enough here, so there is no
            // length.
            addSkipLine = std::to_string(noticeModules) +
                          " of these carry a legal notice from their maker: " + noticeNames +
                          ". Each notice is on that module's DATA PLATE below.";
        }

        const bool addAckRow = noticeModules > 0 && !model.addAllRunning;
        const float addAckH = addAckRow ? (tinyH + cascade::gui::px(12.0f)) : 0.0f;
        float addTextH = noteHeight(addNoteW, addLead.c_str());
        if (!addSkipLine.empty()) {
            addTextH += 4.0f + noteHeight(addNoteW, addSkipLine.c_str());
        }
        if (!model.addAllSummary.empty()) {
            addTextH += 4.0f + noteHeight(addNoteW, model.addAllSummary.c_str());
        }
        addAllH = kPad + std::max(addKeyH + addAckH, addTextH) + kPad;
        addAllTotal = addAllH + kGap;
        addAllWellDrawn_ = true;

        ImGui::Dummy(ImVec2(width, addAllTotal));
        {
            const ImVec2 tl(origin.x, origin.y);
            const ImVec2 br(tl.x + width, tl.y + addAllH);
            addDeckWell(dl, tl, br);
            dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                             true);
            const ImVec2 kTL(tl.x + kPad, tl.y + kPad);
            if (drawDeckKey(dl, kTL, ImVec2(kTL.x + addKeyW, kTL.y + addKeyH),
                            plan.label.c_str(), nullptr,
                            plan.blockedReason.empty() && !model.addAllRunning, "addall")) {
                addAll_ = true;
            }
            if (addAckRow) {
                // A REAL TICK, not a rocker: this is a consent and it reads as one
                // everywhere else in this application.
                ImGui::SetCursorScreenPos(ImVec2(kTL.x + 2.0f, kTL.y + addKeyH + 6.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kCream));
                ImGui::PushFont(uf, tiny);
                char ack[96];
                std::snprintf(ack, sizeof ack, "I accept the %d legal notice%s above",
                              noticeModules, noticeModules == 1 ? "" : "s");
                ImGui::Checkbox(ack, &deck.addAllAck);
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }
            float ny = tl.y + kPad;
            drawNote(dl, ImVec2(tl.x + kPad * 2.0f + addKeyW, ny), addNoteW, addAccent,
                     addLead.c_str());
            ny += noteHeight(addNoteW, addLead.c_str());
            if (!addSkipLine.empty()) {
                ny += 4.0f;
                drawNote(dl, ImVec2(tl.x + kPad * 2.0f + addKeyW, ny), addNoteW, theme::kGold,
                         addSkipLine.c_str());
                ny += noteHeight(addNoteW, addSkipLine.c_str());
            }
            if (!model.addAllSummary.empty()) {
                // WHAT THE RUN ACTUALLY DID, left on the panel after it ends -
                // "23 installed, 0 failed", or the names that failed with the
                // reason each of them gave, verbatim.
                ny += 4.0f;
                drawNote(dl, ImVec2(tl.x + kPad * 2.0f + addKeyW, ny), addNoteW,
                         model.addAllFailed ? theme::kAlarm : theme::kPhosphor,
                         model.addAllSummary.c_str());
            }
            dl->PopClipRect();
        }
    }

    // ======================= THE UPDATES BANNER =============================
    //
    // NOT "HELD". The design's banner says two updates are held because they
    // replace a module that is currently decoding, and offers one key that
    // fits both. This application holds nothing: PluginRepo::planUpdates is a
    // pure function of the catalogue and the manifest, applyUpdate runs the
    // moment the user presses a key, and there is no decoding test anywhere on
    // that path. A banner saying "held" would describe a mechanism that does
    // not exist, and the reason it gave would be an invention.
    //
    // So the banner reports what IS true - what the catalogue offers, whether
    // anything has been read at all, and what pressing UPDATE does - and the
    // key is per module, because one transfer at a time is the rule the
    // repository actually enforces.
    //
    // `catState` is computed above the ADD ALL well now, not here - the same
    // value, read once.
    const char* bannerCaption;
    const char* bannerNote;
    ImU32 bannerLamp;
    bool bannerLit;
    if (catState == CatalogueState::Bundled) {
        // NOT A LAMP AND NOT AN ALARM. Nothing is wrong, nothing is pending
        // and nothing is missing: this is what the product IS on this
        // platform, so the banner states it and asks for nothing.
        //
        // ONE LINE, AND IT NOW CARRIES THE ADD ALL WELL'S OWN REASON TOO -
        // the well itself is not drawn in this state (see the note above it),
        // so "nothing is fetched or added" is doing the same job its dead
        // key's sentence used to: saying why there is no fetch to offer. Kept
        // to one line at the store's own prose size on the narrowest real
        // body this window has been measured on (1174 px, docked) - the
        // caption above ("MODULES BUNDLED WITH THIS BUILD") and the list
        // heading below already say what is bundled and where it is, so this
        // sentence only has to say the one new thing: nothing here fetches or
        // installs, on any day, on any device.
        bannerCaption = "MODULES BUNDLED WITH THIS BUILD";
        bannerLamp = theme::kGold;
        bannerLit = false;
        bannerNote = "Bundled with this build - nothing is fetched or added.";
    } else if (catState == CatalogueState::NeverAsked) {
        bannerCaption = "CATALOGUE NOT READ";
        bannerLamp = theme::kGold;
        bannerLit = false;
        bannerNote =
            "Nothing has been fetched, so nothing here is a count of what exists. This "
            "application does not contact the catalogue on its own - press CHECK NOW and "
            "it will ask once.";
    } else if (catState == CatalogueState::Failed) {
        // ASKED, AND IT DID NOT ANSWER. Telling this user to press CHECK NOW
        // is telling them to do again the thing that just failed, so the
        // banner says what happened and points at the reason instead.
        bannerCaption = "LAST CHECK FAILED";
        bannerLamp = theme::kAlarm;
        bannerLit = true;
        bannerNote =
            "The last check did not return a catalogue, so nothing here is a count of "
            "what exists. The reason it gave is printed under CATALOGUE SOURCE, word for "
            "word. Nothing is retried on its own.";
    } else if (catState == CatalogueState::ReadEmpty) {
        // ANSWERED, AND THE ANSWER WAS NONE. That is a fact about the
        // catalogue, not a state to keep pressing CHECK NOW against.
        bannerCaption = "CATALOGUE IS EMPTY";
        bannerLamp = theme::kGold;
        bannerLit = true;
        bannerNote =
            "The catalogue was read and it lists no modules at all. Nothing is hidden by "
            "the switches below - there is nothing to hide - and this is the whole answer "
            "until the catalogue itself changes.";
    } else if (updateCount == 0) {
        bannerCaption = "NO UPDATES";
        bannerLamp = theme::kPhosphor;
        bannerLit = true;
        bannerNote =
            "The catalogue was read and no fitted module has a newer build in it. Nothing "
            "updates on its own, so this is the whole answer until you check again.";
    } else {
        bannerCaption = "UPDATES AVAILABLE";
        bannerLamp = theme::kGold;
        bannerLit = true;
        // WHAT ACTUALLY HAPPENS TO THE BYTES. This sentence said the key
        // "checks its signature". Nothing in this product verifies a
        // signature, and a security guarantee invented on a panel is the worst
        // kind of copy there is: the user leans on it precisely where they can
        // least afford to. What PluginRepo::install does is listed instead,
        // including the one thing the sha256 does NOT prove.
        bannerNote =
            "Available, not held: nothing defers an update here, and nothing applies one "
            "unasked. Each key below fetches that build over https, refuses it unless the "
            "bytes hash to the sha256 the catalogue published, and only then moves it into "
            "the modules folder and reloads - one at a time, because one transfer at a "
            "time is all the downloader does. That digest comes from the same catalogue "
            "as the file: it proves the download arrived unaltered, and it is not a "
            "signature and vouches for nobody.";
    }

    const float capW = kBannerCapW;
    const float bannerNoteW = width - capW - kPad * 3.0f - cascade::gui::px(12.0f);
    const float bannerHeadH =
        std::max(cascade::gui::px(20.0f), noteHeight(bannerNoteW, bannerNote));
    const float updKeyW = kUpdKeyW;
    const float updNoteW = width - kPad * 2.0f - updKeyW - cascade::gui::px(12.0f);

    std::vector<int> updRows;
    for (int i = 0; i < static_cast<int>(model.modules.size()); ++i) {
        if (!model.modules[static_cast<std::size_t>(i)].updateToVersion.empty()) {
            updRows.push_back(i);
        }
    }
    float updBlockH = 0.0f;
    std::vector<float> updRowH(updRows.size(), 0.0f);
    for (std::size_t k = 0; k < updRows.size(); ++k) {
        const StoreModule& sm = model.modules[static_cast<std::size_t>(updRows[k])];
        const float textH = faceH(uf, uiPx) + 3.0f + tinyH + 3.0f +
                            wrapH(uf, tiny, updNoteW, sm.updateReason.c_str());
        updRowH[k] = std::max(kKeyH + cascade::gui::px(6.0f), textH) + cascade::gui::px(14.0f);
        updBlockH += updRowH[k] + cascade::gui::px(6.0f);
    }
    const float bannerH = kPad + bannerHeadH + (updRows.empty() ? 0.0f : (8.0f + updBlockH)) +
                          kPad;

    ImGui::Dummy(ImVec2(width, bannerH));
    {
        // BELOW THE ADD ALL WELL, not at the page's own origin: the two wells
        // are stacked and a banner still drawn at origin.y would simply paint
        // over the key. (It did, on the first run of this page.)
        const ImVec2 tl(origin.x, origin.y + addAllTotal);
        const ImVec2 br(tl.x + width, tl.y + bannerH);
        addDeckWell(dl, tl, br);
        const float lampR = cascade::gui::px(6.0f);
        const ImVec2 lampC(tl.x + kPad + lampR, tl.y + kPad + lampR + 2.0f);
        drawBenchLamp(dl, lampC, lampR, bannerLamp, bannerLit, nullptr);
        // THE WORD IS DRAWN WHATEVER THE LAMP DOES. A state carried by colour
        // alone is unreadable in a greyscale photograph and to about one man
        // in twelve.
        dl->AddText(lf, tiny, ImVec2(lampC.x + lampR + 8.0f, lampC.y - legH * 0.5f),
                    bannerLit ? bannerLamp : theme::kInkMuted, bannerCaption);
        if (updateCount > 0) {
            char n[16];
            std::snprintf(n, sizeof n, "%d", updateCount);
            dl->AddText(rf, tiny,
                        ImVec2(lampC.x + lampR + 8.0f, lampC.y - legH * 0.5f + legH + 3.0f),
                        theme::kAmber, n);
            dl->AddText(uf, tiny,
                        ImVec2(lampC.x + lampR + 8.0f + textW(rf, tiny, n) + 4.0f,
                               lampC.y - legH * 0.5f + legH + 3.0f),
                        theme::kInkMuted, updateCount == 1 ? "MODULE" : "MODULES");
        }
        addBenchDivider(dl, tl.x + kPad + capW - 10.0f, tl.y + kPad,
                        tl.y + kPad + bannerHeadH);
        drawNote(dl, ImVec2(tl.x + kPad + capW, tl.y + kPad), bannerNoteW,
                 bannerLit ? bannerLamp : theme::kInkMuted, bannerNote);

        float y = tl.y + kPad + bannerHeadH + 8.0f;
        for (std::size_t k = 0; k < updRows.size(); ++k) {
            const int idx = updRows[k];
            const StoreModule& sm = model.modules[static_cast<std::size_t>(idx)];
            const ImVec2 rTL(tl.x + kPad, y);
            const ImVec2 rBR(br.x - kPad, y + updRowH[k]);
            addPlateBox(dl, rTL, rBR);
            float ry = rTL.y + 7.0f;
            dl->AddText(uf, uiPx, ImVec2(rTL.x + 10.0f, ry), theme::kIvory,
                        sm.plate.name.c_str());
            ry += faceH(uf, uiPx) + 3.0f;
            // FROM and TO, both drawn: an update that only names where it is
            // going does not let anyone tell a step from a leap.
            float vx = rTL.x + 10.0f;
            dl->AddText(rf, tiny, ImVec2(vx, ry), theme::kInkMuted,
                        sm.plate.version.c_str());
            vx += textW(rf, tiny, sm.plate.version.c_str()) + 8.0f;
            dl->AddText(uf, tiny, ImVec2(vx, ry), theme::kInkFaint, "to");
            vx += textW(uf, tiny, "to") + 8.0f;
            dl->AddText(rf, tiny, ImVec2(vx, ry), theme::kAmber,
                        sm.updateToVersion.c_str());
            ry += tinyH + 3.0f;
            if (!sm.updateReason.empty()) {
                // PluginUpdate::reason, verbatim - it is user-facing copy the
                // planner already wrote, and two wordings of one decision is
                // how a product comes to give two answers.
                dl->AddText(uf, tiny, ImVec2(rTL.x + 10.0f, ry), theme::kInkMuted,
                            sm.updateReason.c_str(), nullptr, updNoteW);
            }
            char keyId[24];
            std::snprintf(keyId, sizeof keyId, "upd%d", idx);
            const ImVec2 kTL(rBR.x - 10.0f - updKeyW, rTL.y + (updRowH[k] - kKeyH) * 0.5f);
            if (drawDeckKey(dl, kTL, ImVec2(kTL.x + updKeyW, kTL.y + kKeyH), "UPDATE",
                            nullptr, !model.busy, keyId)) {
                updateIndex_ = idx;
            }
            y += updRowH[k] + 6.0f;
        }
    }

    // ======================= THE CONTROL DECK ===============================
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + addAllTotal + bannerH + kGap));
    const ImVec2 deckTL = ImGui::GetCursorScreenPos();
    const float wellW = (width - kGap * 2.0f) / 3.0f;
    const float wellInner = wellW - kPad * 2.0f;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float fieldH = uiPx + style.FramePadding.y * 2.0f + cascade::gui::px(6.0f);
    const char* searchLegend = "Searches name, maker and description.";

    const char* showNote =
        "Three states and three kinds, and every module is in exactly one of each. NOT "
        "DECLARED is not a gap in this window: the catalogue index carries no capability "
        "field, so a module's kind is only known once it is fitted.";
    // THE SAME TWO FACTS, SHORTER - used ONLY in `compact` below, never in the
    // ordinary case, so the desktop at UI scale 1.0 keeps the sentence above
    // verbatim. Both wordings say the states-and-kinds partition is disjoint
    // and exhaustive and both name the reason NOT DECLARED exists; this one
    // says it in fewer words because it is the last thing standing between
    // the deck and gui::pageDeckHeightCap() on the docked tablet.
    const char* showNoteCompact = "Three states, three kinds. NOT DECLARED: no capability yet.";
    // TWO COLUMNS WHEN THEY FIT, ONE WHEN THEY DO NOT. A rocker whose label
    // plate has been squeezed off the row is a switch nobody can read, so the
    // well grows taller rather than letting that happen.
    //
    // AND "FIT" IS MEASURED FROM THE LONGEST LABEL, not from 74. A rocker's
    // plate is drawn at the label's own width and is never clipped, so a
    // column too narrow does not shorten the word - it puts the plate out
    // through the switch beside it and through the count on the right-hand end
    // of the row. At 14 px the longest of these six needs about 82 px of
    // column before its count, so 74 was already the wrong side of the line
    // and said so nowhere.
    //
    // THE COUNT RESERVE IS TWO DIGITS, NOT THREE, and this is a measured
    // change and not a fudge: the largest of the six groups this rocker can
    // ever report is bounded by model.modules.size(), and the largest
    // catalogue this window has ever been handed a figure for is 27 (the
    // desktop's live index, post-Android) against 14 bundled on Android. Two
    // digits covers up to 99 - more than triple the biggest catalogue this
    // product has shipped.
    //
    // A FUNCTION OF THE LABEL SIZE, NOT A CONSTANT, because compact mode
    // (below) letters these labels a size down on the narrowest real bodies
    // this window has been measured on. Measured on the docked tablet
    // emulator's ACTUAL body (1174x906 - see the note on `compact`, not the
    // 1860x1400 this window was first briefed against): even after every
    // other figure in this well went compact, a single column of six rows at
    // the full-size label was 544 px tall on its own, more than the entire
    // deck's half-body allowance - so the label itself has to give up size
    // before two columns can exist at all in a well this narrow.
    auto showRockerMinWAt = [&](float labelPx) {
        return cascade::gui::px(16.0f) + cascade::gui::px(7.0f) +
              std::max({textW(uf, labelPx, "NOT DECLARED"), textW(uf, labelPx, "OTHER KINDS"),
                        textW(uf, labelPx, "NOT FITTED"), textW(uf, labelPx, "CANNOT FIT"),
                        textW(uf, labelPx, "DECODERS"), textW(uf, labelPx, "FITTED")}) +
              cascade::gui::px(12.0f) + cascade::gui::px(6.0f) + textW(rf, labelPx, "00");
    };

    // WHERE THE MODULES CAME FROM, in one line. "no catalogue source set" is a
    // configuration fault on the desktop and would be a lie here: a bundled
    // build has a source, and the source is the application.
    const std::string sourceLine =
        model.bundled
            ? std::string("this build - the modules are inside it, nothing is fetched")
        : model.sourceUrl.empty() ? std::string("no catalogue source set")
                                  : model.sourceUrl;
    // THE SAME FACT, SHORTER - used ONLY in `compact` below, never in the
    // ordinary case, for the same reason showNoteCompact exists: this is the
    // one line CATALOGUE SOURCE's own well has room for at the docked
    // tablet's real width even after the well's own compaction, and a URL
    // (the non-bundled case) is not this window's to shorten, so only the
    // bundled wording gets a second, terser version.
    const std::string sourceLineCompact =
        model.bundled ? std::string("this build - nothing is fetched") : sourceLine;

    // THE SEARCH WELL'S OWN FLOOR, measured rather than felt: the CLEAR key at
    // its own measured width, a gap, and room to read back a short module
    // name while typing it - "ADS-B" is a real one, not a round number, and
    // this field's job is exactly to narrow the list by a name or a maker
    // (see searchLegend just above: "Searches name, maker and description.").
    // Only spent when `compact` below says the deck needs it; the search
    // well is exactly a third of the page otherwise, unchanged from before
    // this existed. Measured tight rather than generous on purpose: on the
    // narrowest real body this window has been measured on (1174x906,
    // docked), the SHOW well needs everything the search well can spare
    // before its own two columns fit at all - see `showRockerMinWAt`.
    const float searchFloorInner =
        kClearW + cascade::gui::px(8.0f) + textW(uf, uiPx, "ADS-B") + cascade::gui::px(16.0f);

    // ONE FORMULA FOR THE THREE WELLS' HEIGHT, CALLED TWICE - once to find
    // out whether their usual, full-size explanatory prose at an equal-thirds
    // split fits under gui::pageDeckHeightCap(), and again (at `notePx` and
    // the possibly-narrower `searchInnerW` below, which the first call's own
    // verdict decides) to lay out the wells for real. A second,
    // independently-typed copy of this arithmetic is exactly the kind of
    // drift the rest of this window measures rather than guesses at - see
    // storeCheckKeyWidth() and moduleKindTagWidth() for the same reasoning
    // applied to a single figure rather than a whole well.
    //
    // `notePx` covers the wrapped explanatory sentences (this well's own
    // caption paragraph, the catalogue source line, its status and its
    // error) AND, as of the real-body measurement above, the SHOW well's own
    // rocker labels and row height - never the field, the keys or the
    // captions, which are short, functional, and stay at the size every
    // other control in this window reads at. A rocker's label is the one
    // exception to "interactive controls keep their size": kTinySize is
    // already the size every OTHER control label in this application reads
    // at (rail keys, bank keys - see fonts.hpp) and it is the store itself
    // that deliberately upsizes to read as paragraphs; when the page cannot
    // afford that upsize, the rocker reads at the size the rest of the
    // application already considers legible, not below it.
    struct DeckWellHeights {
        float deckAH = 0.0f;
        float deckBH = 0.0f;
        float deckCH = 0.0f;
        bool showTwoCols = false;
        float showRows = 6.0f;
        float showColW = 0.0f;
        float rockerH = 0.0f;
        float searchLegendH = 0.0f;
        float srcLineH = 0.0f;
        float srcTextW = 0.0f;
    };
    auto computeWellHeights = [&](float notePx, float searchInnerW, float showInnerW,
                                  float sortInnerW, const char* showNoteText,
                                  const std::string& sourceLineText, bool compactCall) {
        DeckWellHeights r;
        // WRAPPED, NOT ASSUMED SINGLE-LINE - this sentence is drawn wrapped to
        // its own well's width, narrow enough at a large enough face to take
        // more than one line, and the line count it takes is not fixed
        // across scales or across the search well's own width.
        // THE WELL'S OWN TOP/BOTTOM PADDING SHRINKS TOO, ONLY ON THE
        // compactCall PASS - the same trade rockerH's own padding makes just
        // below, and for the same real-body reason: px(10.0f) is comfortable
        // breathing room around a well's content, and every well on every
        // OTHER page still gets it, but on the docked tablet's real body it
        // was six pixels of margin the deck did not have (two wells, top and
        // bottom, at the difference between px(10.0f) and px(4.0f) each).
        const float wellPad =
            compactCall ? cascade::gui::px(4.0f) : kPad;
        r.searchLegendH = wrapH(uf, notePx, searchInnerW, searchLegend);
        r.deckAH = wellPad + legH + 8.0f + fieldH + 9.0f + r.searchLegendH + 4.0f +
                  countLineHeight() + wellPad;

        // THE ROCKER'S OWN PADDING SHRINKS TOO, ONLY ON THE compactCall PASS:
        // px(10.0f) is generous breathing room around a full-size label, and
        // it is what every OTHER page's rocker still gets. On the real docked
        // body (1174x906) six rows' worth of it was 6 px more than the
        // difference between fitting under gui::pageDeckHeightCap() and not -
        // a smaller label already reads with room to spare at px(4.0f), so
        // the extra was spent on nothing.
        r.rockerH = std::max(cascade::gui::px(22.0f),
                             faceH(uf, notePx) +
                                 (compactCall ? cascade::gui::px(4.0f)
                                              : cascade::gui::px(10.0f)));
        r.showColW = (showInnerW - cascade::gui::px(12.0f)) * 0.5f;
        r.showTwoCols = r.showColW >= showRockerMinWAt(notePx);
        r.showRows = r.showTwoCols ? 3.0f : 6.0f;
        r.deckBH = wellPad + legH + 8.0f + r.rockerH * r.showRows + 8.0f +
                  noteHeightAt(showInnerW, showNoteText, notePx) + wellPad;

        // px() ON THE GAP, from the scaling slice: every figure in this
        // window is in scaled pixels, and the bundled line is drawn in the
        // same well as the url it replaces.
        r.srcTextW = sortInnerW - kCheckW - cascade::gui::px(8.0f);
        r.srcLineH = std::max(kKeyH, wrapH(uf, notePx, r.srcTextW, sourceLineText.c_str()));
        // THE RAIL'S OWN CLEARANCE TIGHTENS TOO, ONLY ON THE compactCall PASS
        // - the gap either side of the hairline between the sort segments and
        // CATALOGUE SOURCE, which is breathing room rather than anything
        // measured from a face. `sortGap` is read back by the drawing code
        // below (the same name, same value) so the two cannot disagree.
        const float sortGap = compactCall ? 6.0f : 12.0f;
        const float railGap = compactCall ? 5.0f : 10.0f;
        r.deckCH = wellPad + legH + 8.0f + kSegH + sortGap + 1.0f + railGap + legH + 8.0f +
                  r.srcLineH + wellPad;
        if (!model.sourceStatus.empty()) {
            r.deckCH += 6.0f + wrapH(uf, notePx, sortInnerW, model.sourceStatus.c_str());
        }
        if (!model.sourceError.empty()) {
            r.deckCH += 6.0f + noteHeightAt(sortInnerW, model.sourceError.c_str(), notePx);
        }
        if (model.busy) { r.deckCH += 6.0f + 14.0f + 4.0f + kKeyH; }
        return r;
    };

    const DeckWellHeights atFullProse =
        computeWellHeights(tiny, wellInner, wellInner, wellInner, showNote, sourceLine, false);
    const float deckH_atFullProse =
        std::max(atFullProse.deckAH, std::max(atFullProse.deckBH, atFullProse.deckCH));
    // COMPACT: the deck's usual equal-thirds split, at this window's usual
    // full prose size, would push the whole upper deck (the ADD ALL well, the
    // state banner already drawn above by this point, and these three wells)
    // past gui::pageDeckHeightCap() - which pushes the MODULE LIST and the
    // DATA PLATE below it, the whole reason this window exists. Two things
    // give way together, and only as far as each is measured to need to:
    // the SEARCH well gives up width down to its own floor (searchFloorInner
    // above) and the SHOW well takes it, since the SHOW well's caption and
    // rocker rows are what a narrow well costs the most; and every wrapped
    // explanatory sentence across all three wells drops to `notePx`. Nothing
    // about this triggers on the desktop: at UI scale 1.0 the trial above is
    // already comfortably under the cap at every width this window can be
    // dragged to (proven down to its own 640 px floor), so `compact` is
    // provably always false there and every existing pinned figure at scale
    // 1.0 - including the equal three-way well split - is untouched.
    const bool compact = (addAllTotal + bannerH + kGap + deckH_atFullProse + kGap) >
                         cascade::gui::pageDeckHeightCap(height);
    const float notePx = compact ? cascade::gui::px(fonts::kTinySize) : tiny;
    const float searchBorrow =
        compact ? std::max(0.0f, wellInner - searchFloorInner) : 0.0f;
    const float searchInnerFinal = wellInner - searchBorrow;
    const float showInnerFinal = wellInner + searchBorrow;
    const DeckWellHeights wh =
        compact ? computeWellHeights(notePx, searchInnerFinal, showInnerFinal, wellInner,
                                     showNoteCompact, sourceLineCompact, true)
                : atFullProse;

    const float deckAH = wh.deckAH;
    const float deckBH = wh.deckBH;
    const float deckCH = wh.deckCH;
    const bool showTwoCols = wh.showTwoCols;
    const float showRows = wh.showRows;
    const float showColW = wh.showColW;
    const float rockerH = wh.rockerH;
    const float searchLegendH = wh.searchLegendH;
    const float srcLineH = wh.srcLineH;
    // THE SAME wellPad computeWellHeights used, so the space the three wells'
    // drawing blocks start their content at (just below) agrees with the
    // space their own height was measured against above.
    const float wellPad = compact ? cascade::gui::px(4.0f) : kPad;
    const float srcTextW = wh.srcTextW;
    // THE THREE WELLS' OWN WIDTHS - equal thirds unless `compact` moved
    // width from SEARCH to SHOW, per the note above.
    const float searchWellW = searchInnerFinal + kPad * 2.0f;
    const float showWellW = showInnerFinal + kPad * 2.0f;
    const float sortWellW = wellInner + kPad * 2.0f;

    const float deckH = std::max(deckAH, std::max(deckBH, deckCH));
    // THE WHOLE UPPER DECK, MEASURED HERE AND NOWHERE ELSE - see
    // PluginStoreView::upperDeckHeight() in the header. Exactly
    // bodyTL.y - origin.y below: addAllTotal already carries the gap after
    // the ADD ALL well (addAllTotal = addAllH + kGap), one more kGap sits
    // between the banner and THE CONTROL DECK (the SetCursorScreenPos that
    // produced deckTL), and a third sits between the three wells and the
    // body (the SetCursorScreenPos that produces bodyTL, right after this).
    upperDeckHeight_ = addAllTotal + bannerH + kGap + deckH + kGap;
    ImGui::Dummy(ImVec2(width, deckH));

    // ---- CATALOGUE SEARCH ---------------------------------------------------
    {
        const ImVec2 tl(deckTL.x, deckTL.y);
        const ImVec2 br(tl.x + searchWellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + wellPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), searchInnerFinal, "CATALOGUE SEARCH");
        y += legH + 8.0f;

        const float clearW = kClearW;
        const ImVec2 fTL(tl.x + kPad, y);
        const ImVec2 fBR(fTL.x + searchInnerFinal - clearW - 8.0f, y + fieldH);
        drawFreqDrumWell(dl, fTL, fBR);
        // THE QUERY IS LETTERED IVORY, not amber. Amber in this palette is a
        // READING - something the radio or the machine measured - and what the
        // user typed is a control. The count beneath it is the reading.
        ImGui::SetCursorScreenPos(ImVec2(fTL.x + 7.0f, fTL.y + 3.0f));
        ImGui::SetNextItemWidth(fBR.x - fTL.x - 14.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kIvory));
        // The field's PLACEHOLDER, which is the only instruction the search
        // gives before anything is typed - muted rather than faint for that.
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(theme::kInkMuted));
        ImGui::PushFont(uf, uiPx);
        ImGui::InputTextWithHint("##search", "type to narrow the catalogue", deck.search,
                                 sizeof deck.search);
        ImGui::PopFont();
        ImGui::PopStyleColor(5);

        if (drawDeckKey(dl, ImVec2(fBR.x + 8.0f, y),
                        ImVec2(fBR.x + 8.0f + clearW, y + fieldH), "CLEAR", nullptr,
                        deck.search[0] != '\0', "clear")) {
            deck.search[0] = '\0';
        }
        y += fieldH + 9.0f;
        dl->AddText(uf, notePx, ImVec2(tl.x + kPad, y), theme::kInkMuted, searchLegend,
                    nullptr, searchInnerFinal);
        y += searchLegendH + 4.0f;
        // WHAT IS ON SCREEN AND WHAT EXISTS, both. "3 shown" alone cannot tell
        // a short catalogue from a filter that is hiding most of it.
        drawCountLine(dl, ImVec2(tl.x + kPad, y), static_cast<int>(visible.size()),
                      static_cast<int>(model.modules.size()),
                      (catState == CatalogueState::Read ||
                       catState == CatalogueState::Bundled)
                          ? "MODULES SHOWN"
                          : "MODULES KNOWN");
        dl->PopClipRect();
    }

    // ---- SHOW ---------------------------------------------------------------
    {
        const ImVec2 tl(deckTL.x + searchWellW + kGap, deckTL.y);
        const ImVec2 br(tl.x + showWellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + wellPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), showInnerFinal, "SHOW");
        y += legH + 8.0f;

        int nFitted = 0;
        int nAvail = 0;
        int nBlocked = 0;
        int nDec = 0;
        int nOther = 0;
        int nUndec = 0;
        for (const StoreModule& sm : model.modules) {
            switch (stateGroup(sm)) {
                case StateGroup::Fitted: ++nFitted; break;
                case StateGroup::Available: ++nAvail; break;
                case StateGroup::Blocked: ++nBlocked; break;
            }
            switch (kindGroup(sm.plate)) {
                case KindGroup::Decoder: ++nDec; break;
                case KindGroup::Other: ++nOther; break;
                case KindGroup::Undeclared: ++nUndec; break;
            }
        }
        const float colW = showTwoCols ? showColW : showInnerFinal;
        struct Row {
            const char* label;
            bool* flag;
            int count;
            const char* id;
        };
        const Row rows[6] = {
            {"FITTED", &deck.showFitted, nFitted, "sf"},
            {"DECODERS", &deck.showDecoders, nDec, "sd"},
            {"NOT FITTED", &deck.showAvailable, nAvail, "sa"},
            {"OTHER KINDS", &deck.showOtherKinds, nOther, "so"},
            {"CANNOT FIT", &deck.showBlocked, nBlocked, "sb"},
            {"NOT DECLARED", &deck.showUndeclared, nUndec, "su"},
        };
        for (int i = 0; i < 6; ++i) {
            // px() ON THE COLUMN GAP TOO. This literal was the one figure in
            // the second column's position that never went through gui::px()
            // - every desktop pixel here does, per ui_scale.hpp's own rule -
            // so at UI scale 2.0 the gap the layout RESERVED (showColW above
            // subtracts px(12.0f)) and the gap actually DRAWN diverged: 24 px
            // reserved, 12 drawn, and the second column sat 12 scaled pixels
            // closer to the first than the space allotted for it. Harmless in
            // that it never overlapped anything - showColW's own reservation
            // covers it - but a gap that quietly stops scaling with everything
            // beside it is exactly the fault this application keeps a whole
            // header (gui/ui_scale.hpp) to prevent.
            const float rx = tl.x + kPad + ((showTwoCols && i % 2 == 1)
                                                ? (colW + cascade::gui::px(12.0f))
                                                : 0.0f);
            const float ry =
                y + rockerH * static_cast<float>(showTwoCols ? (i / 2) : i);
            char cnt[16];
            std::snprintf(cnt, sizeof cnt, "%d", rows[i].count);
            if (drawRockerRowAt(dl, ImVec2(rx, ry), colW, rockerH, rows[i].label, cnt,
                               *rows[i].flag, rows[i].id, notePx)) {
                *rows[i].flag = !*rows[i].flag;
            }
        }
        y += rockerH * showRows + 8.0f;
        drawNoteAt(dl, ImVec2(tl.x + kPad, y), showInnerFinal, theme::kInkMuted,
                  compact ? showNoteCompact : showNote, notePx);
        dl->PopClipRect();
    }

    // ---- SORT and CATALOGUE SOURCE -----------------------------------------
    {
        const ImVec2 tl(deckTL.x + searchWellW + kGap + showWellW + kGap, deckTL.y);
        const ImVec2 br(tl.x + sortWellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + wellPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellInner, "SORT");
        y += legH + 8.0f;
        // THREE SEGMENTS, NOT A MENU: the whole option set visible at once, so
        // the current order is legible without opening anything.
        const float segW = (wellInner - 8.0f) / 3.0f;
        for (int i = 0; i < kStoreSortCount; ++i) {
            const ImVec2 sTL(tl.x + kPad + (segW + 4.0f) * static_cast<float>(i), y);
            char id[8];
            std::snprintf(id, sizeof id, "srt%d", i);
            if (drawSegment(dl, sTL, ImVec2(sTL.x + segW, sTL.y + kSegH),
                            storeSortLabel(i), sortKey == i, id)) {
                deck.sortKey = i;
            }
        }
        // THE SAME sortGap/railGap computeWellHeights used, so the drawn
        // clearance around the rail agrees with what its height was
        // measured against above.
        const float sortGap = compact ? 6.0f : 12.0f;
        const float railGap = compact ? 5.0f : 10.0f;
        y += kSegH + sortGap;
        addBenchRail(dl, tl.x + kPad, br.x - kPad, y);
        y += railGap;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellInner, "CATALOGUE SOURCE");
        y += legH + 8.0f;

        // WHERE THE MODULES WOULD COME FROM, printed before the key that goes
        // and gets them. A store that will not say what it is about to contact
        // is asking for a decision it has withheld the facts for.
        dl->AddText(uf, notePx, ImVec2(tl.x + kPad, y), theme::kInkMuted,
                    (compact ? sourceLineCompact : sourceLine).c_str(), nullptr, srcTextW);
        if (drawDeckKey(dl, ImVec2(br.x - kPad - kCheckW, y),
                        ImVec2(br.x - kPad, y + kKeyH),
                        // AGAIN once anything has been asked, whatever came
                        // back. A failed check and an empty catalogue have
                        // both been asked, and a key still saying NOW invites
                        // the user to do again what they just did.
                        //
                        // AND NO CATALOGUE AT ALL in a bundled build. The key
                        // is kept rather than removed - the well's layout
                        // reserves its width, and a control that vanishes
                        // answers no question - but it is engraved with what
                        // is true and is dead. WHY is on the line beside it
                        // (sourceLine) and in the banner above, in sentences,
                        // rather than crammed onto a key: "NO CATALOGUE" is
                        // the whole of what a key can honestly say.
                        catState == CatalogueState::Bundled      ? "NO CATALOGUE"
                        : catState == CatalogueState::NeverAsked ? "CHECK NOW"
                                                                 : "CHECK AGAIN",
                        nullptr,
                        !model.bundled && !model.busy && !model.sourceUrl.empty(),
                        "checknow")) {
            checkNow_ = true;
        }
        y += srcLineH;

        if (model.busy) {
            y += 6.0f;
            // PluginRepo::progress() stays at 0 when the server sends no
            // Content-Length. The bar then simply does not move rather than
            // inventing a figure, and the label says what is moving.
            const ImVec2 pTL(tl.x + kPad, y);
            const ImVec2 pBR(br.x - kPad, y + 14.0f);
            dl->AddRectFilled(pTL, pBR, theme::kVoid, 2.0f);
            const float frac = std::clamp(model.progress, 0.0f, 1.0f);
            if (frac > 0.0f) {
                dl->AddRectFilled(pTL, ImVec2(pTL.x + (pBR.x - pTL.x) * frac, pBR.y),
                                  theme::kAmber, 2.0f);
            }
            dl->AddRect(pTL, pBR, theme::withAlpha(theme::kBrassMid, 0.9f), 2.0f, 0,
                        theme::kHairline);
            if (!model.busyLabel.empty()) {
                dl->AddText(uf, tiny, ImVec2(pTL.x + 6.0f, pTL.y + 1.0f), theme::kCream,
                            model.busyLabel.c_str(), nullptr, pBR.x - pTL.x - 12.0f);
            }
            y += 14.0f + 4.0f;
            if (drawDeckKey(dl, ImVec2(tl.x + kPad, y), ImVec2(br.x - kPad, y + kKeyH),
                            "CANCEL", nullptr, true, "cancel")) {
                cancel_ = true;
            }
            y += kKeyH;
        }
        if (!model.sourceStatus.empty()) {
            y += 6.0f;
            dl->AddText(uf, notePx, ImVec2(tl.x + kPad, y), theme::kInkMuted,
                        model.sourceStatus.c_str(), nullptr, wellInner);
            y += wrapH(uf, notePx, wellInner, model.sourceStatus.c_str());
        }
        if (!model.sourceError.empty()) {
            // VERBATIM AND IN RUST. A private repository answers 404, a TLS
            // failure says so, and the text PluginRepo wrote is the only
            // evidence the user has.
            y += 6.0f;
            drawNoteAt(dl, ImVec2(tl.x + kPad, y), wellInner, theme::kAlarm,
                      model.sourceError.c_str(), notePx);
        }
        dl->PopClipRect();
    }

    // ======================= THE BODY =======================================
    ImGui::SetCursorScreenPos(ImVec2(origin.x, deckTL.y + deckH + kGap));
    const ImVec2 bodyTL = ImGui::GetCursorScreenPos();
    // THE LIST'S FLOOR IS MEASURED IN ROWS AND IS SCALED - and READ THE SECOND
    // HALF OF THIS NOTE, because it does not fix the fault that found it.
    //
    // 120.0f UNSCALED WAS WRONG ON ITS OWN TERMS: every other figure in this
    // window went through gui::px() with the scaling slice, and this was the
    // floor of the one region the window exists for. So it is derived from
    // what a row costs - the MODULES heading plus three cards, a card being at
    // least its action column (a key, the install word, the state word) and
    // its padding - which holds three rows at any face size and any UI scale.
    //
    // WHAT IT DOES NOT FIX, stated here because a reader will otherwise assume
    // it does. Measured on the Pixel Tablet emulator, 2560x1600 at UI scale
    // x2: the deck above - the ADD ALL well, the banner, and the three wells
    // whose shared height is the tallest of them - is TALLER THAN THE WHOLE
    // PAGE, so `height - bodyTL.y` is negative and the module list and the
    // data plate are drawn past the bottom edge. They cannot be reached by
    // scrolling either, and that is not an oversight in the page: the window
    // beginPage() opens and the ##storeface child the view is drawn into are
    // BOTH ImGuiWindowFlags_NoScrollbar | NoScrollWithMouse (app_window.cpp),
    // so this view's contract is to fit the box it is given. It currently
    // cannot at that scale, and the window reports "14 OF 14 MODULES SHOWN"
    // above a list nobody can see.
    //
    // It was harmless while Android had no catalogue and therefore no rows.
    // The moment the modules are bundled INTO the build, that list is the only
    // place they are described - so this needs a decision (let the face
    // scroll, or make the deck shorter at large scales) rather than a floor.
    const float bodyFloor = nameH + kPad * 2.0f + (kKeyH + cascade::gui::px(26.0f)) * 3.0f;
    const float bodyH = std::max(bodyFloor, origin.y + height - bodyTL.y);
    // The plate takes a third, but never at the cost of a list too narrow to
    // read a module name in - the list is what this window is FOR, and a plate
    // beside three characters of name would be the tail wagging the dog.
    //
    // THE FLOOR AND THE CEILING BOTH ROSE WITH THE FACE. The plate is a column
    // of wrapped sentences - the reach rows most of all - and 260 px of it at
    // 21 px is four or five words a line, which is a paragraph nobody reads.
    // The list keeps its 400 px floor for the same reason: the longest name in
    // the live catalogue is "406 MHz Distress Beacon Decoder (EPIRB / ELT /
    // PLB)" at fifty-one characters, and it now WRAPS rather than being cut at
    // the column edge, so the column has to be wide enough for that to be two
    // lines and not six.
    // NOT PUT THROUGH px() HERE, AND DELIBERATELY LEFT THAT WAY FOR NOW. It
    // reads like the same fault fabc498 and the note above moduleRowColumns()
    // both fixed - `width` arrives in scaled device pixels and these four
    // literals do not - but scaling all four moves BOTH the plate's own floor
    // and the list's floor at once, and on the real docked body (1174 px)
    // the two floors already cannot both be honoured (280 + 400 + the gap
    // exceeds 1174 at scale 2): whichever is scaled "correctly" changes WHICH
    // window loses width it was promised, not whether one of them does. That
    // is a real decision about this window's priorities under a body neither
    // floor was written for, not a units bug with one honest fix - and it is
    // the reason this line is named here rather than changed. See
    // moduleRowColumns() below: it is written to answer honestly (an empty
    // text column, never an overlapping one) for whatever `cw` this formula
    // hands it, which is what makes leaving this one alone safe for now.
    const float plateW =
        std::min(std::clamp(width * 0.33f, 340.0f, 560.0f),
                 std::max(280.0f, width - kGap - 400.0f));
    const float listW = width - plateW - kGap;

    // ---- the module list ----------------------------------------------------
    {
        const ImVec2 tl = bodyTL;
        const ImVec2 br(tl.x + listW, tl.y + bodyH);
        addDeckWell(dl, tl, br);
        float y = tl.y + kPad;
        dl->AddText(lf, uiPx, ImVec2(tl.x + kPad, y), theme::kIvory, "MODULES");
        {
            char cnt[24];
            std::snprintf(cnt, sizeof cnt, "%d", static_cast<int>(visible.size()));
            const float cw = textW(rf, tiny, cnt) + 6.0f + textW(uf, tiny, "SHOWN");
            dl->AddText(rf, tiny, ImVec2(br.x - kPad - cw, y + nameH - faceH(rf, tiny)),
                        theme::kAmber, cnt);
            dl->AddText(uf, tiny,
                        ImVec2(br.x - kPad - cw + textW(rf, tiny, cnt) + 6.0f,
                               y + nameH - faceH(uf, tiny)),
                        theme::kInkMuted, "SHOWN");
        }
        y += nameH + 6.0f;
        addBenchRail(dl, tl.x + kPad, br.x - kPad, y);
        y += 8.0f;

        const float childH = br.y - y - kPad;
        ImGui::SetCursorScreenPos(ImVec2(tl.x + kPad, y));
        // px() ON THE LAST-DITCH FLOOR TOO. It can only be reached if the well
        // itself was squeezed below its own floor, but 40 desktop pixels on a
        // x2 tablet is half a line of the face it would be showing.
        ImGui::BeginChild("##modlist",
                          ImVec2(listW - kPad * 2.0f,
                                 std::max(cascade::gui::px(40.0f), childH)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const float cw = std::max(120.0f, ImGui::GetContentRegionAvail().x);
            if (visible.empty()) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                // WHY THE LIST IS EMPTY, and there are five reasons, not two.
                // "Press CHECK NOW" is the right answer to exactly one of
                // them; said to the other four it sends the user round a loop
                // that cannot end, because the check has already happened - or
                // because there is no key to press at all.
                const char* why = "";
                switch (catState) {
                    case CatalogueState::Bundled:
                        // AN EMPTY LIST IN A BUNDLED BUILD IS A REAL FAULT,
                        // and the only one this window can report: the modules
                        // are inside the application, so either none was
                        // packaged for this machine's architecture or the host
                        // could not find the directory it was told they are
                        // in. Both are answered in the FITTED MODULES window,
                        // which names the directory it scanned - so the user
                        // is sent there rather than at a key.
                        why = (hiddenByShow > 0 || deck.search[0] != '\0')
                                  ? "Every bundled module is hidden by the SHOW switches "
                                    "or the search above. The counts on the switches say "
                                    "how many each holds."
                                  : "This build lists no modules at all. They are compiled "
                                    "into the application, so this means none was loaded: "
                                    "the FITTED MODULES window names the directory that "
                                    "was scanned and what was found in it.";
                        break;
                    case CatalogueState::NeverAsked:
                        why = "No catalogue has been read yet. Press CHECK NOW above and "
                              "this application asks the source once.";
                        break;
                    case CatalogueState::Failed:
                        why = "The last check did not return a catalogue, so there is "
                              "nothing to list. The reason it gave is under CATALOGUE "
                              "SOURCE above, word for word.";
                        break;
                    case CatalogueState::ReadEmpty:
                        why = "The catalogue was read and it lists no modules at all. "
                              "Nothing here is hidden by the switches above.";
                        break;
                    case CatalogueState::Read:
                        why = (hiddenByShow > 0 || deck.search[0] != '\0')
                                  ? "Every module is hidden by the SHOW switches or the "
                                    "search above. The counts on the switches say how many "
                                    "each holds."
                                  : "No module in the catalogue matches.";
                        break;
                }
                drawNote(cdl, at, cw - 8.0f, theme::kGold, why);
                ImGui::Dummy(ImVec2(cw, noteHeight(cw - 8.0f, why)));
            }
            // BOTH FIXED COLUMNS MEASURED FROM THEIR OWN WORDS, and the split
            // between them and the text column is moduleRowColumns() now, not
            // a second copy of the same arithmetic - see its own comment for
            // why: a floor that could claim more width than the card actually
            // had left was what let this row's own text run under the action
            // column on the docked tablet's real body.
            const float kTagW = moduleKindTagWidth();
            const float kActW = moduleActionColumnWidth();
            const float kCardPad = cascade::gui::px(14.0f);
            for (int idx : visible) {
                const StoreModule& sm = model.modules[static_cast<std::size_t>(idx)];
                const ModulePlate& p = sm.plate;
                const bool isSel = idx == deck.selected;
                const ModuleRowColumns cols = moduleRowColumns(cw);
                const float midW = cols.midW;
                const std::string reach = moduleReachSummary(p);
                // THE SUMMARY ON THE ROW, THE DESCRIPTION ON THE PLATE. See
                // ModulePlate::summary: the live catalogue's descriptions run
                // to three thousand characters, and a row that wrapped one was
                // eleven lines tall - so the list showed ONE module of
                // twenty-four and the rest were a scroll away. Nothing is cut
                // to make this fit; the shorter of the two fields is simply
                // the one a list is for.
                const std::string& rowText = p.summary.empty() ? p.blurb : p.summary;
                const StoreInstallState instState = storeInstallState(sm);
                const char* instWord = storeInstallWord(instState);

                // The maker/licence foot line, built here rather than in the
                // drawing block below because whether the reach summary fits
                // BESIDE it decides the row's height. Measuring from one string
                // and drawing another is how a row comes to clip itself.
                char foot[192];
                std::snprintf(foot, sizeof foot, "%s  \xc2\xb7  %s",
                              p.maker.empty() ? "maker not stated" : p.maker.c_str(),
                              p.licence.empty() ? "no licence declared" : p.licence.c_str());
                const bool reachBeside =
                    textW(uf, tiny, foot) + 14.0f + textW(uf, tiny, reach.c_str()) <
                    midW + 6.0f;

                // WHY THE FIT KEY ON THIS ROW IS DEAD. A greyed key with no
                // sentence beside it is the fault this window exists to
                // remove, and until now the row's key was drawn dead from
                // blockedReason with nothing to explain it - the plate said
                // why, one selection away, for whichever module happened to be
                // on it. The reason belongs on the row that refuses.
                const std::string blockedLine =
                    (!p.fitted && !sm.blockedReason.empty())
                        ? ("Cannot fit: " + sm.blockedReason)
                        : std::string();

                // THE NAME WRAPS NOW, and that is the truncation this change
                // set out to remove. It used to be laid end to end with the
                // version and the state word and CLIPPED to this column, which
                // on the longest name in the live catalogue - "406 MHz
                // Distress Beacon Decoder (EPIRB / ELT / PLB)", fifty-one
                // characters - cut it at "(EPIR". A name is the one string on
                // the card a user matches against what they were looking for,
                // so it is the last one that may be cut.
                const char* nameText =
                    p.name.empty() ? "(unnamed module)" : p.name.c_str();
                const float rowNameH = wrapH(lf, uiPx, midW, nameText);
                const float idLineH = std::max(faceH(rf, tiny), tinyH);
                const float rowsH =
                    rowNameH + 3.0f + idLineH + 6.0f +
                    (rowText.empty() ? 0.0f : wrapH(uf, tiny, midW, rowText.c_str()) + 6.0f) +
                    tinyH + (reachBeside ? 0.0f : tinyH + 2.0f) +
                    (blockedLine.empty() ? 0.0f : noteHeight(midW, blockedLine.c_str()) + 4.0f);
                // The action column: the key, then the INSTALL word beneath it
                // and the running state word beneath that - each WRAPPED to the
                // column, because "NOT INSTALLED" and "TAKES NO SIGNAL" do not
                // fit on one line there and a word running out over the card's
                // edge is worse than a word on two lines.
                const char* stateWord = moduleStateWord(p);
                const float stateWordW = std::max(40.0f, kActW - 18.0f);
                const float actH = kKeyH + 8.0f + wrapH(uf, tiny, stateWordW, instWord) +
                                   6.0f + wrapH(uf, tiny, stateWordW, stateWord);
                const float cardH = std::max(rowsH, actH) + kCardPad * 2.0f;

                const ImVec2 cTL = ImGui::GetCursorScreenPos();
                const ImVec2 cBR(cTL.x + cw, cTL.y + cardH);

                ImGui::PushID(idx);
                ImGui::SetCursorScreenPos(cTL);
                // THE WHOLE ROW SELECTS, AND THE KEY ON IT STILL WORKS. Without
                // AllowOverlap the card's hit area claims the hover first and
                // every key drawn inside it afterwards is dead - the button is
                // visibly there, takes the pointer, and does nothing.
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##card", ImVec2(cw, cardH))) {
                    deck.selected = idx;
                    deck.legalAck = false;
                }
                const bool hovered = ImGui::IsItemHovered();

                cdl->AddRectFilled(cTL, cBR, isSel ? theme::kEnamel : theme::kWell,
                                   theme::kKeyRounding);
                cdl->AddRect(cTL, cBR,
                             isSel ? theme::kBrassBright
                                   : theme::withAlpha(theme::kBrassDark,
                                                      hovered ? 1.0f : 0.75f),
                             theme::kKeyRounding, 0, theme::kHairline);
                if (isSel) {
                    // The selected row is picked out with a rust bar, which is
                    // the one place rust is not trouble: it is the cursor, not
                    // a reading and not a fault.
                    cdl->AddRectFilled(cTL, ImVec2(cTL.x + 3.0f, cBR.y), theme::kAlarm);
                }

                // --- the kind tag ------------------------------------------
                {
                    const float tagPx = cascade::gui::px(fonts::kTinySize);
                    const ImVec2 tTL(cTL.x + kCardPad, cTL.y + kCardPad);
                    const ImVec2 tBR(tTL.x + kTagW,
                                     tTL.y + faceH(uf, tagPx) + cascade::gui::px(6.0f));
                    cdl->AddRectFilled(tTL, tBR, theme::kBrassBright, 1.0f);
                    addBenchBevel(cdl, tTL, tBR, 1.0f, true);
                    const char* tag = moduleKindTag(p);
                    // THE CHIP KEEPS THE TINY FACE and moduleKindTagWidth's own
                    // measurement, because it is SHARED with the FITTED MODULES
                    // window: one chip drawn two sizes in two windows is exactly
                    // the inconsistency that function was written to end. It is
                    // a category label on metal, not a sentence.
                    cdl->AddText(uf, tagPx,
                                 ImVec2((tTL.x + tBR.x) * 0.5f -
                                            textW(uf, tagPx, tag) * 0.5f,
                                        tTL.y + cascade::gui::px(3.0f)),
                                 theme::kEnamel, tag);
                }

                const float mx = cTL.x + cols.mx;
                const float ax = cTL.x + cols.ax;
                float my = cTL.y + kCardPad;
                // WRAPPED, NOT CLIPPED. midW is the same width the card's
                // height was measured from, so what is drawn and what was
                // measured cannot disagree - and a name too long for one line
                // takes a second rather than being cut mid-word.
                cdl->AddText(lf, uiPx, ImVec2(mx, my), isSel ? theme::kIvory : theme::kCream,
                             nameText, nullptr, midW);
                my += rowNameH + 3.0f;
                {
                    // THE VERSION AND THE INSTALL STATE, on their own line and
                    // at the page's own size. Both used to be squeezed onto the
                    // end of the name line in the smallest engraving the
                    // application has, and both are what a user is actually
                    // scanning the list for.
                    // CLIPPED TO THE TEXT COLUMN, per-glyph, the same
                    // technique railPlateLabel and centreDockTabLabel use for
                    // arbitrary third-party text with nothing else bounding
                    // it: on a card narrow enough that midW is genuinely
                    // small, "1.0.0" plus "INSTALLED" drawn end to end with
                    // no limit at all is exactly what used to run under the
                    // action column's own key.
                    const ImVec4 idClip(mx, my, ax, my + idLineH);
                    float vx = mx;
                    if (!p.version.empty()) {
                        cdl->AddText(rf, tiny, ImVec2(vx, my + idLineH - faceH(rf, tiny)),
                                     theme::kAmber, p.version.c_str(), nullptr, 0.0f, &idClip);
                        vx += textW(rf, tiny, p.version.c_str()) + 16.0f;
                    }
                    cdl->AddText(uf, tiny, ImVec2(vx, my + idLineH - tinyH),
                                 storeInstallColour(instState), instWord, nullptr, 0.0f,
                                 &idClip);
                }
                my += idLineH + 6.0f;
                if (!rowText.empty()) {
                    cdl->AddText(uf, tiny, ImVec2(mx, my), theme::kCream, rowText.c_str(),
                                 nullptr, midW);
                    my += wrapH(uf, tiny, midW, rowText.c_str()) + 6.0f;
                }
                {
                    // Maker and licence on the ROW, not only on the plate: the
                    // terms a module arrives under are part of choosing it,
                    // not a detail to discover after fitting.
                    // MUTED, NOT FAINT. This line is the maker and the licence
                    // - the terms the module arrives under - and the comment
                    // above says why they are on the row at all. A line worth
                    // putting there is a line worth being able to read.
                    //
                    // CLIPPED, per-glyph, to the text column - `foot` is a
                    // maker's own name and licence string, third-party text
                    // with nothing else bounding it, and `reachBeside`'s own
                    // check only ever asked whether foot-plus-reach fit
                    // TOGETHER; a foot long enough on its own (a real maker
                    // name plus a real licence identifier easily clears 160 px
                    // at the docked tablet's own width) still ran under the
                    // action column with nothing to stop it.
                    const ImVec4 footClip(mx, my, ax, my + tinyH);
                    cdl->AddText(uf, tiny, ImVec2(mx, my),
                                 p.licence.empty() ? theme::kGold : theme::kInkMuted, foot,
                                 nullptr, 0.0f, &footClip);
                    if (reachBeside) {
                        cdl->AddText(uf, tiny,
                                     ImVec2(mx + textW(uf, tiny, foot) + 14.0f, my),
                                     moduleReachColour(p), reach.c_str(), nullptr, 0.0f,
                                     &footClip);
                        my += tinyH;
                    } else {
                        const ImVec4 reachClip(mx, my + tinyH + 2.0f, ax,
                                              my + tinyH + 2.0f + tinyH);
                        cdl->AddText(uf, tiny, ImVec2(mx, my + tinyH + 2.0f),
                                     moduleReachColour(p), reach.c_str(), nullptr, 0.0f,
                                     &reachClip);
                        my += tinyH + tinyH + 2.0f;
                    }
                }
                if (!blockedLine.empty()) {
                    // BESIDE THE KEY THAT REFUSED, on the same row, in the
                    // same words the plate uses for the same fact. Gold, not
                    // rust: a module this machine cannot fit is not a fault,
                    // it is a thing to read.
                    my += 4.0f;
                    drawNote(cdl, ImVec2(mx, my), midW, theme::kGold, blockedLine.c_str());
                }

                // --- the action key and the running lamp --------------------
                {
                    const float ay = cTL.y + kCardPad;
                    const bool hasUpdate = !sm.updateToVersion.empty();
                    if (!p.fitted) {
                        if (drawDeckKey(cdl, ImVec2(ax, ay), ImVec2(ax + kActW, ay + kKeyH),
                                        "FIT", nullptr, sm.blockedReason.empty(), "fit")) {
                            fitIndex_ = idx;
                        }
                    } else if (hasUpdate) {
                        if (drawDeckKey(cdl, ImVec2(ax, ay), ImVec2(ax + kActW, ay + kKeyH),
                                        "UPDATE", nullptr, !model.busy, "upd")) {
                            updateIndex_ = idx;
                        }
                    } else {
                        // NO REMOVE AND NO STOP HERE, deliberately. This window
                        // is the catalogue; running, stopping and removing a
                        // fitted module belong to the FITTED MODULES window, and
                        // two windows offering the same control is how they come
                        // to disagree about what it did.
                        drawDeckKey(cdl, ImVec2(ax, ay), ImVec2(ax + kActW, ay + kKeyH),
                                    "FITTED", nullptr, false, "fitted");
                    }
                    // THE STATE WORD AND ITS LAMP, from the shared component,
                    // so this row and the plate beside it cannot describe one
                    // module two ways - and so neither of them says RUNNING,
                    // which this side has no way to test. STARTED is what is
                    // known here; whether anything reaches the module is the
                    // FITTED MODULES window's answer, and it is handed the
                    // runner and the receiver to give it.
                    //
                    // THE INSTALL WORD GOES FIRST, directly under the key, in
                    // the theme's own ink for that state: it answers the
                    // question this window is FOR ("have I got this, and is it
                    // current"), and the running state below answers a
                    // different one.
                    float ly = ay + kKeyH + 8.0f;
                    cdl->AddText(uf, tiny, ImVec2(ax, ly), storeInstallColour(instState),
                                 instWord, nullptr, stateWordW);
                    ly += wrapH(uf, tiny, stateWordW, instWord) + 6.0f;
                    const ImVec2 lampC(ax + 6.0f, ly + tinyH * 0.5f);
                    drawBenchLamp(cdl, lampC, 4.5f, moduleStateColour(p),
                                  moduleStateLampLit(p), nullptr);
                    cdl->AddText(uf, tiny, ImVec2(lampC.x + 9.0f, ly), moduleStateColour(p),
                                 stateWord, nullptr, stateWordW);
                }
                ImGui::PopID();
                ImGui::SetCursorScreenPos(ImVec2(cTL.x, cBR.y + 10.0f));
                ImGui::Dummy(ImVec2(cw, 0.0f));
            }
        }
        ImGui::EndChild();
    }

    // ---- the data plate -----------------------------------------------------
    {
        const ImVec2 tl(bodyTL.x + listW + kGap, bodyTL.y);
        const ImVec2 br(tl.x + plateW, tl.y + bodyH);
        addDeckWell(dl, tl, br);
        float y = tl.y + kPad;
        dl->AddText(lf, uiPx, ImVec2(tl.x + kPad, y), theme::kIvory, "DATA PLATE");
        y += nameH + 6.0f;
        addBenchRail(dl, tl.x + kPad, br.x - kPad, y);
        y += 8.0f;

        const float childH = br.y - y - kPad;
        ImGui::SetCursorScreenPos(ImVec2(tl.x + kPad, y));
        // Scaled, for the same reason as the module list's own floor above.
        ImGui::BeginChild("##plate",
                          ImVec2(plateW - kPad * 2.0f,
                                 std::max(cascade::gui::px(40.0f), childH)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const float pw = std::max(120.0f, ImGui::GetContentRegionAvail().x);
            if (deck.selected < 0 ||
                deck.selected >= static_cast<int>(model.modules.size())) {
                const char* none = "";
                switch (catState) {
                    case CatalogueState::Bundled:
                        none = "Select a module on the left to read what it is. Every one "
                               "of them is compiled into this build.";
                        break;
                    case CatalogueState::NeverAsked:
                        none = "Nothing to describe yet. Press CHECK NOW to read the "
                               "catalogue.";
                        break;
                    case CatalogueState::Failed:
                        none = "Nothing to describe: the last check did not return a "
                               "catalogue. Its reason is under CATALOGUE SOURCE.";
                        break;
                    case CatalogueState::ReadEmpty:
                        none = "Nothing to describe: the catalogue was read and it lists "
                               "no modules.";
                        break;
                    case CatalogueState::Read:
                        none = "Nothing selected. Pick a module on the left and its plate "
                               "is drawn here.";
                        break;
                }
                drawNote(pdl, ImGui::GetCursorScreenPos(), pw - 6.0f, theme::kInkMuted,
                         none);
                ImGui::Dummy(ImVec2(pw, noteHeight(pw - 6.0f, none)));
            } else {
                const StoreModule& sm =
                    model.modules[static_cast<std::size_t>(deck.selected)];
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float h = drawModuleDataPlate(pdl, at, pw - 6.0f, sm.plate);
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h + 10.0f));

                // --- the acknowledgement gate, then the key -----------------
                if (!sm.plate.legalNotice.empty() && !sm.plate.fitted) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kCream));
                    ImGui::PushFont(uf, uiPx);
                    ImGui::Checkbox("I have read the notice above and accept responsibility",
                                    &deck.legalAck);
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                }

                const ImVec2 kTL = ImGui::GetCursorScreenPos();
                const bool hasUpdate = !sm.updateToVersion.empty();
                // The gate the DESKTOP already applies, plus this window's own
                // acknowledgement. blockedReason comes from the one predicate
                // the existing button uses, so the sentence under this key and
                // the key itself cannot disagree.
                std::string blocked = sm.blockedReason;
                if (blocked.empty() && !sm.plate.legalNotice.empty() && !sm.plate.fitted &&
                    !deck.legalAck) {
                    blocked = "the legal notice must be acknowledged first";
                }
                if (!sm.plate.fitted) {
                    if (drawDeckKey(pdl, kTL, ImVec2(kTL.x + pw - 6.0f, kTL.y + kPlateKeyH),
                                    "FIT MODULE", nullptr, blocked.empty(), "platefit")) {
                        fitIndex_ = deck.selected;
                    }
                } else if (hasUpdate) {
                    char to[96];
                    std::snprintf(to, sizeof to, "TO v%s", sm.updateToVersion.c_str());
                    if (drawDeckKey(pdl, kTL, ImVec2(kTL.x + pw - 6.0f, kTL.y + kPlateKeyH),
                                    "UPDATE MODULE", to, !model.busy, "plateupd")) {
                        updateIndex_ = deck.selected;
                    }
                } else {
                    drawDeckKey(pdl, kTL, ImVec2(kTL.x + pw - 6.0f, kTL.y + kPlateKeyH),
                                "ALREADY FITTED", nullptr, false, "platefitted");
                }
                ImGui::SetCursorScreenPos(ImVec2(kTL.x, kTL.y + kPlateKeyH + 8.0f));
                ImGui::Dummy(ImVec2(pw, 0.0f));

                float ny = kTL.y + kPlateKeyH + 8.0f;
                if (!blocked.empty() && !sm.plate.fitted) {
                    // A DEAD KEY ALWAYS SAYS WHY. A greyed control with no
                    // sentence beside it is the fault this whole redesign
                    // exists to remove.
                    const std::string why = "Cannot fit: " + blocked;
                    drawNote(pdl, ImVec2(kTL.x, ny), pw - 6.0f, theme::kGold, why.c_str());
                    ny += noteHeight(pw - 6.0f, why.c_str()) + 8.0f;
                } else if (sm.plate.fitted && !hasUpdate) {
                    const char* note =
                        "Fitted. Starting, stopping and removing it are on the FITTED "
                        "MODULES window - this one is the catalogue.";
                    drawNote(pdl, ImVec2(kTL.x, ny), pw - 6.0f, theme::kPhosphor, note);
                    ny += noteHeight(pw - 6.0f, note) + 8.0f;
                }
                if (!model.resultError.empty()) {
                    // PluginRepo's own words. A sha256 mismatch names both
                    // digests, and paraphrasing it would throw away the only
                    // evidence the user has that the bytes were not the bytes
                    // the catalogue vouched for.
                    drawNote(pdl, ImVec2(kTL.x, ny), pw - 6.0f, theme::kAlarm,
                             model.resultError.c_str());
                    ny += noteHeight(pw - 6.0f, model.resultError.c_str()) + 8.0f;
                }
                if (!model.resultReport.empty()) {
                    drawNote(pdl, ImVec2(kTL.x, ny), pw - 6.0f, theme::kPhosphor,
                             model.resultReport.c_str());
                    ny += noteHeight(pw - 6.0f, model.resultReport.c_str()) + 8.0f;
                }
                ImGui::SetCursorScreenPos(ImVec2(kTL.x, ny));
                ImGui::Dummy(ImVec2(pw, 0.0f));
            }
        }
        ImGui::EndChild();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, bodyTL.y + bodyH));
    ImGui::PopID();
}

}  // namespace cascade::gui
