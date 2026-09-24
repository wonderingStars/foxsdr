// text_fit.hpp - a word fitted to the thing it is written on, and a word
// letter-spaced a CHARACTER at a time.
//
// WHY THIS IS SHARED NOW. Both halves used to live in the anonymous namespaces
// of the panels that needed them (scope_view.cpp, spectrum_view.cpp), each
// written for the English the bench was lettered in. Translation broke both
// at once. A key sized for "DECODE" gets "DÉCODAGE", and Dear ImGui neither
// wraps nor shrinks - the word hangs out over the key's edges and through the
// key beside it. And a tracked caption stepped a BYTE at a time draws each half
// of "É" as its own missing glyph: FUNCTION SELECT in the pseudo language came
// out as "F??????T??????", which is how this file was found. One copy of each,
// correct for UTF-8, is what every surface draws through.
//
// FITTING, THE RULE. A word that fits is drawn exactly as it always was - the
// same size, the same place - so English, which every box here was measured
// against, does not move by a pixel. A word that does not fit is drawn
// SMALLER, measured at the real face, down to a floor; only a word too long
// even at the floor is cut, at the edge of the thing it is written on, because
// a truncated label spoils itself and an overflowing one spoils its neighbour.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_TEXT_FIT_HPP
#define CASCADE_GUI_TEXT_FIT_HPP

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <initializer_list>
#include <string>

#include "core/utf8_text.hpp"
#include "imgui.h"

namespace cascade::gui {

// THE ABSOLUTE FLOOR. Below about nine pixels an engraved capital is not a
// smaller caption, it is dirt on the panel - drawBenchStopButton says the same
// about its own word, and the top bar's captions stop here too.
inline constexpr float kFitFloorPx = 9.0f;

// The floor for a word that normally sits at `px`: seven tenths of its own
// size, and never under the absolute floor. A caption allowed all the way down
// to nine pixels from seventeen would fit anything and be read by nobody;
// seven tenths is the most a key can give before its word stops looking like
// the same kind of word as the keys beside it.
inline float fitFloorFor(float px) { return std::max(kFitFloorPx, px * 0.7f); }

// THE ARITHMETIC, with the measurement handed in: `widthAt(size)` is the
// word's width at a size. The size at which it fits `room`, or `px` when it
// already does, or `floorPx` when nothing above the floor would.
//
// IT MEASURES AGAIN AFTER SHRINKING, and that is not belt and braces. A
// glyph's advance is rasterised at the size it is asked for and lands on a
// whole pixel, so a word's width is very nearly - but not exactly - linear in
// the size, and the one division that ought to land on the answer can leave
// it a pixel or two over its room. Four passes is far more than the rounding
// ever needs and cannot loop.
template <class WidthAt>
float fitSize(float px, float room, WidthAt&& widthAt, float floorPx = kFitFloorPx) {
    if (!(room > 0.0f) || !(px > 0.0f)) { return px; }
    float out = px;
    for (int pass = 0; pass < 4; ++pass) {
        const float w = widthAt(out);
        if (!(w > room) || !(w > 0.0f)) { break; }
        const float next = std::max(floorPx, out * room / w - 0.05f);
        if (!(next < out)) { break; }  // at the floor, and the cut takes over
        out = next;
    }
    return out;
}

// The size at which `text` fits `room` in `font`.
inline float fitTextPx(ImFont* font, float px, const char* text, float room,
                       float floorPx = kFitFloorPx) {
    if (font == nullptr || text == nullptr || text[0] == '\0') { return px; }
    return fitSize(
        px, room, [&](float s) { return font->CalcTextSizeA(s, FLT_MAX, 0.0f, text).x; },
        floorPx);
}

// LETTER-SPACING, WHICH DEAR IMGUI HAS NOT. Several of the bench's captions
// are tracked out - FUNCTION SELECT, SIGNAL PATH, the plate titles - and
// tracking is most of what separates an engraved legend from a word in a
// label. There is no style var for it, so the glyphs are drawn one at a time
// with the advance added by hand.
inline float trackedWidth(ImFont* font, float px, const char* text, float tracking) {
    if (font == nullptr || text == nullptr) { return 0.0f; }
    float w = 0.0f;
    int glyphs = 0;
    // A CHARACTER at a time, not a byte: "É" is two bytes and one glyph.
    for (const char* p = text; *p != '\0'; p = cascade::core::utf8Next(p)) {
        w += font->CalcTextSizeA(px, FLT_MAX, 0.0f, p, cascade::core::utf8Next(p)).x;
        ++glyphs;
    }
    if (glyphs > 1) { w += tracking * static_cast<float>(glyphs - 1); }
    return w;
}

// The size at which a tracked caption, WITH its tracking, fits `room`.
// Tracking is a fraction of the size so both halves of the width scale
// together.
inline float fitTrackedPx(ImFont* font, float px, const char* text, float trackingFrac,
                          float room, float floorPx = kFitFloorPx) {
    if (font == nullptr || text == nullptr || text[0] == '\0') { return px; }
    return fitSize(
        px, room, [&](float s) { return trackedWidth(font, s, text, s * trackingFrac); },
        floorPx);
}

// `maxX` IS THE END OF THE THING THE CAPTION IS WRITTEN ON, and the run stops
// there rather than carrying on past it - the cut that follows a fit which
// reached its floor. Half a pixel of slack, because the shadow pass is drawn
// one pixel right of the cut it sits under and a caption fitted exactly to its
// room lands exactly on this edge.
inline void addTrackedText(ImDrawList* dl, ImFont* font, float px, const ImVec2& at, ImU32 col,
                           const char* text, float tracking, float maxX = FLT_MAX) {
    if (dl == nullptr || font == nullptr || text == nullptr) { return; }
    float x = at.x;
    for (const char* p = text; *p != '\0';) {
        const char* next = cascade::core::utf8Next(p);
        const float adv = font->CalcTextSizeA(px, FLT_MAX, 0.0f, p, next).x;
        if (x + adv > maxX + 1.5f) { break; }
        dl->AddText(font, px, ImVec2(x, at.y), col, p, next);
        x += adv + tracking;
        p = next;
    }
}

// A WORD CENTRED IN A BOX - a key, a chip, a legend - fitted to the box less
// `padX` each side. Returns the size it was drawn at. `yNudge` moves it down
// (a pressed key's word sinks with the key). A word that fits is drawn at
// exactly the position the centring always gave it, with no clip rectangle,
// so nothing about an English key changes; one cut at the floor is clipped to
// the box.
//
// `floorPx` is how small the word may go before it is cut: seven tenths of
// its size when left negative (fitFloorFor), or a floor of the caller's. The
// five rail bank keys pass the absolute kFitFloorPx - a row of five keys
// sharing one rail is the tightest box on the bench, and "JÄRJESTELMÄ" or
// "РАСШИРЕНИЯ" cut to "JÄRJESTELM" / "РАСШИРЕНИ" at seven tenths is a worse
// key than the whole word two pixels smaller (34-language review).
inline float addFittedCentred(ImDrawList* dl, ImFont* font, float px, const ImVec2& tl,
                              const ImVec2& br, ImU32 col, const char* text, float padX,
                              float yNudge = 0.0f, float floorPx = -1.0f) {
    if (dl == nullptr || font == nullptr || text == nullptr || text[0] == '\0') { return px; }
    const float room = (br.x - tl.x) - 2.0f * padX;
    const float size =
        fitTextPx(font, px, text, room, floorPx > 0.0f ? floorPx : fitFloorFor(px));
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 at((tl.x + br.x) * 0.5f - ts.x * 0.5f,
                    (tl.y + br.y) * 0.5f - ts.y * 0.5f + yNudge);
    if (ts.x > room && room > 0.0f) {
        const ImVec4 clip(tl.x + padX, tl.y, br.x - padX, br.y + yNudge);
        dl->AddText(font, size, ImVec2(tl.x + padX, at.y), col, text, nullptr, 0.0f, &clip);
    } else {
        dl->AddText(font, size, at, col, text);
    }
    return size;
}

// --- words that share a cell's size and may not leave the cell -----------------
//
// THE TUNING DECK'S UP / DN STENCILS: two words lettered at one size, tracked
// out, each in a cell a tube wide with its neighbours a few pixels away. The
// order of what gives way: the SIZE first, down to the floor; then the
// TRACKING, which is decoration and costs a fifth of an em per gap; and only
// then is a word cut - and the caller cuts it at a whole glyph inside the
// cell, never at the cell's edge. A Russian "ВВЕРХ" cut at the edge ran its
// last half-letter up to the next tube's stencil and the row read as one
// smeared word (34-language review).
struct CellWordsFit {
    float px;            // the one size every word is lettered at
    float trackingFrac;  // tracking as a fraction of px: the caller's, or 0
    bool fits;           // every word fits the cell whole
};

// `widthAt(i, size, trackingFrac)` is word i's width. English UP / DN fit at
// the caller's size and tracking and come back exactly as asked.
template <class WidthAt>
CellWordsFit fitCellWords(float px, float trackingFrac, float room, float floorPx, int count,
                          WidthAt&& widthAt) {
    CellWordsFit out{px, trackingFrac, true};
    for (const float tf : {trackingFrac, 0.0f}) {
        float s = px;
        for (int i = 0; i < count; ++i) {
            s = std::min(
                s, fitSize(px, room, [&](float z) { return widthAt(i, z, tf); }, floorPx));
        }
        bool ok = true;
        for (int i = 0; i < count; ++i) { ok = ok && widthAt(i, s, tf) <= room + 0.5f; }
        out = CellWordsFit{s, tf, ok};
        if (ok || !(trackingFrac > 0.0f)) { break; }
    }
    return out;
}

// --- one line of text: smaller, and only then wrapped ---------------------------
//
// A STATUS WORD OR A CARD'S SUB-LINE has a line of its own and a width it may
// not run past. The rule is the one above - drawn smaller until it fits - with
// a different last resort: where a word in a key can only be cut, a line that
// is still too long at its floor has room BELOW it, and a sentence cut in the
// middle of a word ("...δεδομ", "PAIGALDAMAT / A") reads as broken. So it is
// wrapped instead, at its own size - words on two lines are still words.
struct LineFit {
    float px;   // the size it is drawn at
    bool wrap;  // too long even at the floor: wrapped at `px` instead
};

// THE DECISION, with the measurement handed in (`widthAt(size)` is the
// text's one-line width at a size). A line that fits is {px, false}, exactly
// as it always was.
template <class WidthAt>
LineFit fitOrWrap(float px, float room, WidthAt&& widthAt, float floorPx) {
    const float s = fitSize(px, room, widthAt, floorPx);
    if (!(room > 0.0f) || widthAt(s) <= room + 0.5f) { return LineFit{s, false}; }
    return LineFit{px, true};
}

inline LineFit fitLine(ImFont* font, float px, const char* text, float room, float floorPx) {
    if (font == nullptr || text == nullptr || text[0] == '\0') { return LineFit{px, false}; }
    return fitOrWrap(
        px, room, [&](float s) { return font->CalcTextSizeA(s, FLT_MAX, 0.0f, text).x; },
        floorPx);
}

// The height the line takes: one line at its OWN size when it is not wrapped
// (a smaller word keeps its line's height, so nothing below it moves), or the
// wrapped block.
inline float fittedLineHeight(ImFont* font, float px, const char* text, float room,
                              const LineFit& fit) {
    if (font == nullptr || text == nullptr || text[0] == '\0') { return 0.0f; }
    if (fit.wrap) { return font->CalcTextSizeA(fit.px, FLT_MAX, std::max(1.0f, room), text).y; }
    return font->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
}

// Draws what fitLine decided at `at` (the top of a line `px` tall). A smaller
// word is centred in that line's height, the way a smaller key word is.
inline void addFittedLine(ImDrawList* dl, ImFont* font, float px, const ImVec2& at, ImU32 col,
                          const char* text, float room, const LineFit& fit) {
    if (dl == nullptr || font == nullptr || text == nullptr || text[0] == '\0') { return; }
    if (fit.wrap) {
        dl->AddText(font, fit.px, at, col, text, nullptr, std::max(1.0f, room));
        return;
    }
    const float lineH = font->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y;
    const float h = font->CalcTextSizeA(fit.px, FLT_MAX, 0.0f, "Ag").y;
    dl->AddText(font, fit.px, ImVec2(at.x, at.y + (lineH - h) * 0.5f), col, text);
}

// --- the longest thing a wrap cannot break -------------------------------------
//
// The widest run of `text` that word wrapping has to keep on one line: a word
// between blanks, or a single Chinese / Japanese character (the cjk-wrap patch
// breaks between any two). A column narrower than this cannot hold the text
// without cutting a word, however many lines it is given.
template <class WidthOf>
float longestUnbreakable(const char* text, WidthOf&& widthOf) {
    if (text == nullptr) { return 0.0f; }
    float best = 0.0f;
    const char* start = nullptr;
    const char* p = text;
    const auto close = [&](const char* end) {
        if (start != nullptr && end > start) { best = std::max(best, widthOf(start, end)); }
        start = nullptr;
    };
    while (*p != '\0') {
        const char* next = cascade::core::utf8Next(p);
        std::size_t i = 0;
        const unsigned int c =
            cascade::core::utf8Decode(std::string_view(p, static_cast<std::size_t>(next - p)), i);
        const bool blank = c == ' ' || c == '\t' || c == '\n' || c == 0x3000;
        const bool cjk = (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
                         (c >= 0xFF00 && c <= 0xFFEF);
        if (blank) {
            close(p);
        } else if (cjk) {
            close(p);
            best = std::max(best, widthOf(p, next));
        } else if (start == nullptr) {
            start = p;
        }
        p = next;
    }
    close(p);
    return best;
}

inline float longestUnbreakableWidth(ImFont* font, float px, const char* text) {
    if (font == nullptr) { return 0.0f; }
    return longestUnbreakable(
        text, [&](const char* a, const char* b) { return font->CalcTextSizeA(px, FLT_MAX, 0.0f, a, b).x; });
}

// --- a label beside its widget, or above it -----------------------------------
//
// ImGui letters a combo's, a slider's or a check box's label to the RIGHT of
// the control, and the bench lays a key beside the field it acts on. Both
// were measured against English on a fixed-width rail; a translation a third
// longer ran off the rail's edge and was cut mid-word. THE RULE (the owner's
// coordinator, 2026-09-24): the second thing moves onto its own line ONLY when
// first + spacing + second does not fit the width there is. When it fits - as
// every English row does today - nothing about the layout changes.

// THE DECISION, and the only arithmetic in it. Half a pixel of slack so a row
// that fits exactly is not moved by float rounding in the measurement.
inline bool fitsBeside(float first, float spacing, float second, float avail) {
    return first + spacing + second <= avail + 0.5f;
}

// The visible part of an ImGui label: everything before "##".
inline const char* visibleEnd(const char* label) {
    const char* h = std::strstr(label, "##");
    return h != nullptr ? h : label + std::strlen(label);
}

// For a widget whose label ImGui draws to its right. Call it immediately
// before the widget and submit the label it returns. When label and widget fit
// side by side the label comes back unchanged. When they do not, the visible
// label is drawn now, on its own line, and what comes back is the widget's ID
// ALONE - "###" and the original label (or the "###id" trId already carries),
// which ImGui hashes exactly as it hashes the original, so the widget's
// identity, open state and focus do not change with the layout.
//
// `widgetW` is the control's own width; left negative it is CalcItemWidth(),
// which honours a SetNextItemWidth the caller made - and that width is set
// again after the label line, which consumed it.
inline const char* labelAboveIfNeeded(const char* label, float widgetW = -1.0f) {
    if (label == nullptr) { return label; }
    const char* end = visibleEnd(label);
    if (end == label) { return label; }  // an id-only label shows nothing
    const float w = widgetW >= 0.0f ? widgetW : ImGui::CalcItemWidth();
    const float labelW = ImGui::CalcTextSize(label, end).x;
    if (fitsBeside(w, ImGui::GetStyle().ItemInnerSpacing.x, labelW,
                   ImGui::GetContentRegionAvail().x)) {
        return label;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(label, end);
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(w);
    // Valid until the next call; the widget is submitted straight after this.
    static std::string idOnly;
    const char* triple = std::strstr(label, "###");
    idOnly = triple != nullptr ? std::string(triple) : std::string("###") + label;
    return idOnly.c_str();
}

// The width ImGui will give a check box or a radio button with this label:
// the square, the inner spacing and the visible text.
inline float checkWidth(const char* label) {
    const char* end = visibleEnd(label);
    const float textW = ImGui::CalcTextSize(label, end).x;
    return ImGui::GetFrameHeight() + (textW > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + textW : 0.0f);
}

// The width of a button sized to its visible label.
inline float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label, visibleEnd(label)).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// SameLine() only when an item `nextW` wide still fits after the one just
// drawn; otherwise the next item starts its own line. Returns which it did.
inline bool sameLineIfFits(float nextW) {
    // After an item the cursor stands at the start of the next line, so the
    // row's width is what is available from there, and the item just drawn
    // occupies the row up to its right edge.
    const float lineStart = ImGui::GetCursorScreenPos().x;
    const float first = ImGui::GetItemRectMax().x - lineStart;
    if (!fitsBeside(first, ImGui::GetStyle().ItemSpacing.x, nextW,
                    ImGui::GetContentRegionAvail().x)) {
        return false;
    }
    ImGui::SameLine();
    return true;
}

// A SHORT LINE BESIDE THE ITEM JUST DRAWN - the reason under a dead SEND key -
// when it fits there, at its own size or drawn smaller down to seven tenths of
// it. Returns false, having drawn nothing, when even the smaller line does not
// fit: the caller then letters it on a line of its own. Fits at its own size:
// exactly sameLineIfFits and a TextUnformatted, as before.
inline bool sameLineFittedText(const char* text) {
    if (text == nullptr) { return false; }
    const float lineStart = ImGui::GetCursorScreenPos().x;
    const float first = ImGui::GetItemRectMax().x - lineStart;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float room = ImGui::GetContentRegionAvail().x - first - spacing;
    ImFont* f = ImGui::GetFont();
    const float px = ImGui::GetFontSize();
    if (fitsBeside(first, spacing, ImGui::CalcTextSize(text).x, ImGui::GetContentRegionAvail().x)) {
        ImGui::SameLine();
        ImGui::TextUnformatted(text);
        return true;
    }
    const float s = fitTextPx(f, px, text, room, fitFloorFor(px));
    if (!(room > 0.0f) || f->CalcTextSizeA(s, FLT_MAX, 0.0f, text).x > room + 0.5f) { return false; }
    ImGui::SameLine();
    // Centred on the line the full-size text would have taken.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (px - s) * 0.5f);
    ImGui::PushFont(f, s);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    return true;
}

// A NAME AND ITS KEY ON ONE ROW - the key-binding list, where every action's
// name has its key cap right-aligned beside it. Three answers:
//
//   Beside        name and key fit on one line: the row it always was.
//   BesideWrapped the name does not fit on one line beside the key, but every
//                 word of it does: the name wraps in the room LEFT of the key,
//                 and the key stays on the name's first line. A long action
//                 name used to drop its key onto a line of its own, where it
//                 sat between two names and read as belonging to either
//                 (34-language review: ca, sl, sk, hu, ro, pt-PT, bg, ru, el).
//   Below         the column is so narrow that even one word of the name does
//                 not fit beside the key: the key goes under the name, as
//                 before, rather than cutting a word.
enum class ChipPlace { Beside, BesideWrapped, Below };

inline ChipPlace chipPlacement(float nameW, float longestWordW, float spacing, float chipW,
                               float rowW) {
    if (fitsBeside(nameW, spacing, chipW, rowW)) { return ChipPlace::Beside; }
    if (fitsBeside(longestWordW, spacing, chipW, rowW)) { return ChipPlace::BesideWrapped; }
    return ChipPlace::Below;
}

// A BOX THAT GIVES WAY to the lines under it. A page of fixed height holds a
// text box and, below it, lines whose height a translation decides - a
// sentence that wraps to four lines instead of three, a reason that drops
// under its key. The box is the one thing on such a page that can be shorter
// without losing anything, so it is: `want` tall while everything fits, and
// shorter by exactly the overflow when it does not, down to `floorH`.
// `availFromTop` is the height left on the page at the box's top edge
// (ImGui::GetContentRegionAvail().y there) and `belowH` the height of
// everything under the box, measured on the previous frame - 0 until it has
// been, and then the box is `want`. A page that fits is the page it always
// was, to the pixel.
inline float boxGivingWay(float want, float floorH, float availFromTop, float belowH) {
    if (!(belowH > 0.0f)) { return want; }
    return std::clamp(availFromTop - belowH, std::min(floorH, want), want);
}

// A button whose caption is drawn smaller when it does not fit the button's
// width (`size.x` > 0, or the whole row for -1, as ImGui::Button takes it) -
// a caption moved above its own button would leave an empty key, so a button
// is the one control that shrinks its word instead. Fits: the very same call.
inline bool fittedButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float boxW = size.x;
    if (boxW < 0.0f) { boxW = ImGui::GetContentRegionAvail().x + boxW + 1.0f; }
    if (boxW <= 0.0f) { return ImGui::Button(label, size); }
    const float room = boxW - st.FramePadding.x * 2.0f;
    ImFont* f = ImGui::GetFont();
    const float px = ImGui::GetFontSize();
    const char* end = visibleEnd(label);
    const float w = f->CalcTextSizeA(px, FLT_MAX, 0.0f, label, end).x;
    if (w <= room) { return ImGui::Button(label, size); }
    const float fitted = fitSize(
        px, room, [&](float s) { return f->CalcTextSizeA(s, FLT_MAX, 0.0f, label, end).x; },
        fitFloorFor(px));
    // A fixed-height row keeps its height: the frame is sized for the old
    // line, so the smaller word is centred in the same button.
    const float h = size.y > 0.0f ? size.y : ImGui::GetFrameHeight();
    ImGui::PushFont(f, fitted);
    const bool pressed = ImGui::Button(label, ImVec2(size.x, h));
    ImGui::PopFont();
    return pressed;
}

// AN INPUT FIELD'S HINT, fitted to the field. ImGui letters a placeholder at
// the field's own size and clips it at the frame, so a translated hint a third
// longer than the English stopped mid-word ("... of slee", "katalog") - and so
// did the longest English one. A hint that fits is the very same call; one
// that does not is drawn by hand over the empty field, smaller down to the
// absolute floor and clipped to the frame only below that. The text the user
// types is never resized - only the instruction shown before anything is.
inline bool inputTextWithFittedHint(const char* label, const char* hint, char* buf,
                                    std::size_t bufSize, ImGuiInputTextFlags flags = 0) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float room = ImGui::CalcItemWidth() - st.FramePadding.x * 2.0f;
    ImFont* f = ImGui::GetFont();
    const float px = ImGui::GetFontSize();
    if (hint == nullptr || hint[0] == '\0' || buf == nullptr || buf[0] != '\0' ||
        !(room > 0.0f) || f->CalcTextSizeA(px, FLT_MAX, 0.0f, hint).x <= room) {
        return ImGui::InputTextWithHint(label, hint, buf, bufSize, flags);
    }
    const bool changed = ImGui::InputText(label, buf, bufSize, flags);
    if (buf[0] == '\0') {
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        const float s = fitTextPx(f, px, hint, room, kFitFloorPx);
        const float h = f->CalcTextSizeA(s, FLT_MAX, 0.0f, hint).y;
        const ImVec2 at(mn.x + st.FramePadding.x, (mn.y + mx.y) * 0.5f - h * 0.5f);
        const ImVec4 clip(mn.x + st.FramePadding.x, mn.y, mx.x - st.FramePadding.x, mx.y);
        ImGui::GetWindowDrawList()->AddText(f, s, at, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            hint, nullptr, 0.0f, &clip);
    }
    return changed;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TEXT_FIT_HPP
