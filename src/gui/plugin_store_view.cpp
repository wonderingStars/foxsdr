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
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
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

// --- the vocabulary this window adds ------------------------------------------

// The recessed bay a group of controls sits in: dark enamel cut into the
// panel, a brass lip around it and the bevel lit from below, which is what
// makes it read as a hole rather than as a dark rectangle.
void addDeckWell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return; }
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
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return; }
    dl->AddRectFilled(tl, br, theme::kWell, theme::kKeyRounding);
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.9f), theme::kKeyRounding, 0,
                theme::kHairline);
}

// A labelled brass key, one or two lines. Disabled draws it drained and
// refuses the click - and the sentence saying WHY lives beside it, because a
// greyed key with no explanation is the fault this redesign exists to remove.
bool drawDeckKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* line1,
                 const char* line2, bool enabled, const char* id) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return false; }
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
            dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f),
                              ImVec2(br.x + 1.0f, br.y + 2.0f),
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
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }

    // Engraved into brass, which the palette's rule allows for a caption on
    // metal and forbids for a reading on glass. A dead key letters in the
    // faint ink instead, so it reads as unavailable rather than as unlabelled.
    ImFont* f = fonts::ui();
    const float px = fonts::kTinySize;
    const ImU32 ink = enabled ? theme::kEnamel : theme::kInkFaint;
    const float lh = faceH(f, px);
    const int lines = (line2 != nullptr && line2[0] != '\0') ? 2 : 1;
    float y = (tl.y + br.y) * 0.5f - lh * static_cast<float>(lines) * 0.5f +
              (held ? 1.0f : 0.0f);
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
bool drawRockerRow(ImDrawList* dl, const ImVec2& tl, float width, float rowH,
                   const char* label, const char* trailing, bool on, const char* id) {
    if (dl == nullptr || width < 50.0f || rowH < 10.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##rocker", ImVec2(width, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();
    ImGui::PopID();

    const float rw = 16.0f;
    const ImVec2 rTL(tl.x, tl.y + 1.0f);
    const ImVec2 rBR(tl.x + rw, tl.y + rowH - 1.0f);
    dl->AddRectFilled(rTL, rBR, theme::kVoid, theme::kKeyRounding);
    dl->AddRect(rTL, rBR, theme::withAlpha(theme::kBrassMid, 0.9f), theme::kKeyRounding, 0,
                theme::kHairline);
    const float ph = (rBR.y - rTL.y) * 0.42f;
    const ImVec2 pTL(rTL.x + 2.0f, on ? rTL.y + 2.0f : rBR.y - 2.0f - ph);
    const ImVec2 pBR(rBR.x - 2.0f, pTL.y + ph);
    dl->AddRectFilled(pTL, pBR, on ? theme::kCream : theme::kBrassMid, 1.0f);
    addBenchBevel(dl, pTL, pBR, 1.0f, true);

    ImFont* f = fonts::ui();
    const float px = fonts::kTinySize;
    const float lw = textW(f, px, label);
    const float lh = faceH(f, px);
    const ImVec2 lTL(tl.x + rw + 7.0f, tl.y + (rowH - lh - 5.0f) * 0.5f);
    const ImVec2 lBR(lTL.x + lw + 12.0f, lTL.y + lh + 5.0f);
    if (on) {
        dl->AddRectFilled(lTL, lBR, theme::kBrassBright, theme::kKeyRounding);
        addBenchBevel(dl, lTL, lBR, theme::kKeyRounding, true);
    } else {
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kBrassDark, 0.9f), theme::kKeyRounding,
                    0, theme::kHairline);
    }
    dl->AddText(f, px, ImVec2(lTL.x + 6.0f, lTL.y + 2.0f),
                on ? theme::kEnamel : theme::kCream, label);
    if (hovered) {
        dl->AddRect(lTL, lBR, theme::withAlpha(theme::kBrassBright, 0.7f),
                    theme::kKeyRounding, 0, theme::kHairline);
    }
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 1.0f),
                    ImVec2(tl.x + width + 2.0f, tl.y + rowH + 1.0f), theme::kBrassBright,
                    theme::kKeyRounding, 0, theme::kHairline);
    }
    // HOW MANY ROWS THIS SWITCH IS HOLDING BACK, on the switch itself. A
    // filter that hides things without saying how many is how a user comes to
    // believe the catalogue is short.
    if (trailing != nullptr && trailing[0] != '\0') {
        ImFont* rf = faceForValue(trailing);
        const float rW = textW(rf, px, trailing);
        const float rx = tl.x + width - rW;
        if (rx > lBR.x + 6.0f) {
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
    if (dl == nullptr || br.x - tl.x < 6.0f) { return false; }
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
        dl->AddRectFilled(ImVec2(tl.x + 1.0f, tl.y + 2.0f), ImVec2(br.x + 1.0f, br.y + 2.0f),
                          theme::withAlpha(theme::kVoid, 0.40f), r);
        dl->AddRectFilled(tl, br, hovered ? theme::kBrassBright : theme::kBrassMid, r);
    }
    addBenchBevel(dl, tl, br, r, !selected);
    if (focused) {
        dl->AddRect(ImVec2(tl.x - 2.0f, tl.y - 2.0f), ImVec2(br.x + 2.0f, br.y + 2.0f),
                    theme::kBrassBright, r + 1.0f, 0, theme::kHairline);
    }
    ImFont* f = fonts::ui();
    const float px = fonts::kTinySize;
    dl->AddText(f, px,
                ImVec2((tl.x + br.x) * 0.5f - textW(f, px, label) * 0.5f,
                       (tl.y + br.y) * 0.5f - faceH(f, px) * 0.5f + (selected ? 1.0f : 0.0f)),
                selected ? theme::kCream : theme::kEnamel, label);
    return pressed;
}

// A HATCHED VALUE: there is no source for this figure, and here is the space
// it would occupy if there were. Diagonal ruling rather than a zero, because
// "0 bytes" and "nobody told us the size" are opposite statements.
void addHatch(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < 4.0f || br.y - tl.y < 4.0f) { return; }
    dl->PushClipRect(tl, br, true);
    const float h = br.y - tl.y;
    const ImU32 col = theme::withAlpha(theme::kInkMuted, 0.24f);
    for (float x = tl.x - h; x < br.x; x += 6.0f) {
        dl->AddLine(ImVec2(x, br.y), ImVec2(x + h, tl.y), col, 2.0f);
    }
    dl->PopClipRect();
}

// A note: a coloured rule down the left, a wash behind it and the sentence
// itself. `accent` carries the meaning - phosphor for something working, gold
// for something the user should look at, rust for something refused.
float noteHeight(float width, const char* text) {
    ImFont* f = fonts::ui();
    const float px = fonts::kTinySize;
    if (text == nullptr || text[0] == '\0') { return 0.0f; }
    return wrapH(f, px, width - 12.0f, text) + 9.0f;
}

void drawNote(ImDrawList* dl, const ImVec2& tl, float width, ImU32 accent,
              const char* text) {
    if (dl == nullptr || width < 30.0f || text == nullptr || text[0] == '\0') { return; }
    ImFont* f = fonts::ui();
    const float px = fonts::kTinySize;
    const float h = noteHeight(width, text);
    dl->AddRectFilled(tl, ImVec2(tl.x + width, tl.y + h), theme::withAlpha(accent, 0.10f));
    dl->AddRectFilled(tl, ImVec2(tl.x + 2.0f, tl.y + h), accent);
    dl->AddText(f, px, ImVec2(tl.x + 9.0f, tl.y + 4.0f), accent, text, nullptr,
                width - 12.0f);
}

// "3 OF 11 SHOWN": the figures in the monospaced face and in amber because
// they are readings; the words beside them in the ui face and in ink because
// they are not. One helper so the two never get transcribed the other way
// round at a second call site.
void drawCountLine(ImDrawList* dl, const ImVec2& at, int n, int m, const char* trail) {
    ImFont* uf = fonts::ui();
    ImFont* rf = fonts::reading();
    const float px = fonts::kTinySize;
    char nBuf[16];
    char mBuf[16];
    std::snprintf(nBuf, sizeof nBuf, "%d", n);
    std::snprintf(mBuf, sizeof mBuf, "%d", m);
    const float base = at.y;
    const float uOff = base + (faceH(rf, px) - faceH(uf, px)) * 0.5f;
    float x = at.x;
    dl->AddText(rf, px, ImVec2(x, base), theme::kAmber, nBuf);
    x += textW(rf, px, nBuf);
    dl->AddText(uf, px, ImVec2(x, uOff), theme::kInkFaint, " OF ");
    x += textW(uf, px, " OF ");
    dl->AddText(rf, px, ImVec2(x, base), theme::kAmber, mBuf);
    x += textW(rf, px, mBuf);
    dl->AddText(uf, px, ImVec2(x + 4.0f, uOff), theme::kInkFaint, trail);
}

float countLineHeight() {
    return std::max(faceH(fonts::reading(), fonts::kTinySize),
                    faceH(fonts::ui(), fonts::kTinySize));
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

    PlateFact size{"DOWNLOAD", {}, false, theme::kAmber};
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
    if ((c & CASCADE_CAP_TRACK_SOURCE) != 0u) {
        r.push_back({"Map targets", "Publishes positions the host draws on its map.",
                     false});
    }
    if ((c & CASCADE_CAP_PANEL) != 0u) {
        r.push_back({"A window of its own", "Rows and controls the host draws for it.",
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
        r.push_back({"Map imagery", "Fetches tiles from a server you point it at.", true});
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
    constexpr float kBoxPad = 12.0f;
    constexpr float kBoxGap = 10.0f;

    ImFont* uf = fonts::ui();
    ImFont* lf = fonts::legend();
    const float tiny = fonts::kTinySize;
    const float uiPx = fonts::kUiSize;
    const float tinyH = faceH(uf, tiny);
    const float legH = faceH(lf, tiny);
    const float nameH = faceH(lf, uiPx);
    const float inner = width - kBoxPad * 2.0f;
    if (inner < 60.0f) { return 0.0f; }

    const std::vector<PlateFact> facts = collectFacts(m);
    const std::vector<ReachRow> reach = collectReach(m);
    // WHICH KIND OF "NOT KNOWN" THIS IS. Chosen once, so the pass that
    // measures the box and the pass that letters it cannot pick differently.
    const char* unknownReach = (m.fitted && !m.loaded) ? kReachRefused : kReachUnknown;
    const float colW = (inner - 14.0f) * 0.5f;

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
    const float factRowH = legH + 2.0f + tinyH + 8.0f;
    float box1H = kBoxPad + nameH + 3.0f + tinyH + blurbH + 10.0f + 1.0f + 10.0f +
                  factRowH * static_cast<float>(factRows) + kBoxPad - 8.0f;
    const float homeH = m.homepage.empty() ? 0.0f : (tinyH + 6.0f);
    box1H += homeH;

    // --- box 2: what this module reaches -------------------------------------
    const float markW = 18.0f;
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
        dl->AddText(lf, uiPx, ImVec2(x, y), theme::kIvory,
                    m.name.empty() ? "(unnamed module)" : m.name.c_str());
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
            const float cx = x + (i % 2u == 0u ? 0.0f : (colW + 14.0f));
            const float cy = y + factRowH * static_cast<float>(i / 2u);
            dl->AddText(lf, tiny, ImVec2(cx, cy), theme::kInkFaint, f.key);
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
            dl->AddText(uf, tiny, ImVec2(x, y - 2.0f), theme::kInkFaint, m.homepage.c_str(),
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
                dl->AddText(uf, tiny, ImVec2(x + markW, y), theme::kInkFaint,
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
enum class CatalogueState {
    NeverAsked,  // nothing fetched this session
    Failed,      // asked, and the attempt failed. sourceError says why
    ReadEmpty,   // asked, answered, and the index listed no modules
    Read,        // asked, answered, and there are rows
};

CatalogueState catalogueState(const PluginStoreModel& m) {
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
    if ((c & CASCADE_CAP_PANEL) != 0u) { return "PANEL"; }
    if ((c & (CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_PRESET)) != 0u) { return "CONTROL"; }
    return "MODULE";
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
        return "fetches from a server you choose";
    }
    return "publishes to the host only";
}

ImU32 moduleReachColour(const ModulePlate& m) {
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

// ===========================================================================
// THE WINDOW
// ===========================================================================

void PluginStoreView::draw(float width, float height, const PluginStoreModel& model,
                           PluginStoreDeck& deck) {
    // Cleared first, so a request is answered once or not at all.
    checkNow_ = false;
    cancel_ = false;
    fitIndex_ = -1;
    updateIndex_ = -1;

    ImGui::PushID("pluginstore");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // TOO NARROW TO DRAW HONESTLY, so it says so instead of drawing a squashed
    // deck with controls collapsed to nothing. This is a real operating-system
    // window the user can drag to any size, and a panel that silently omits
    // half its switches at 400px is worse than one that asks to be widened.
    if (width < 560.0f || height < 260.0f) {
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
    const float tiny = fonts::kTinySize;
    const float uiPx = fonts::kUiSize;
    const float tinyH = faceH(uf, tiny);
    const float legH = faceH(lf, tiny);
    const float nameH = faceH(lf, uiPx);

    constexpr float kPad = 10.0f;
    constexpr float kGap = 10.0f;
    constexpr float kKeyH = 28.0f;
    constexpr float kRockerH = 22.0f;
    constexpr float kSegH = 24.0f;

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
    const CatalogueState catState = catalogueState(model);
    const char* bannerCaption;
    const char* bannerNote;
    ImU32 bannerLamp;
    bool bannerLit;
    if (catState == CatalogueState::NeverAsked) {
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

    const float capW = 178.0f;
    const float bannerNoteW = width - capW - kPad * 3.0f - 12.0f;
    const float bannerHeadH = std::max(20.0f, noteHeight(bannerNoteW, bannerNote));
    const float updKeyW = 96.0f;
    const float updNoteW = width - kPad * 2.0f - updKeyW - 12.0f;

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
        updRowH[k] = std::max(kKeyH + 6.0f, textH) + 14.0f;
        updBlockH += updRowH[k] + 6.0f;
    }
    const float bannerH = kPad + bannerHeadH + (updRows.empty() ? 0.0f : (8.0f + updBlockH)) +
                          kPad;

    ImGui::Dummy(ImVec2(width, bannerH));
    {
        const ImVec2 tl(origin.x, origin.y);
        const ImVec2 br(tl.x + width, tl.y + bannerH);
        addDeckWell(dl, tl, br);
        const float lampR = 6.0f;
        const ImVec2 lampC(tl.x + kPad + lampR, tl.y + kPad + lampR + 2.0f);
        drawBenchLamp(dl, lampC, lampR, bannerLamp, bannerLit, nullptr);
        // THE WORD IS DRAWN WHATEVER THE LAMP DOES. A state carried by colour
        // alone is unreadable in a greyscale photograph and to about one man
        // in twelve.
        dl->AddText(lf, tiny, ImVec2(lampC.x + lampR + 8.0f, lampC.y - legH * 0.5f),
                    bannerLit ? bannerLamp : theme::kInkFaint, bannerCaption);
        if (updateCount > 0) {
            char n[16];
            std::snprintf(n, sizeof n, "%d", updateCount);
            dl->AddText(rf, tiny,
                        ImVec2(lampC.x + lampR + 8.0f, lampC.y - legH * 0.5f + legH + 3.0f),
                        theme::kAmber, n);
            dl->AddText(uf, tiny,
                        ImVec2(lampC.x + lampR + 8.0f + textW(rf, tiny, n) + 4.0f,
                               lampC.y - legH * 0.5f + legH + 3.0f),
                        theme::kInkFaint, updateCount == 1 ? "MODULE" : "MODULES");
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
                dl->AddText(uf, tiny, ImVec2(rTL.x + 10.0f, ry), theme::kInkFaint,
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
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + bannerH + kGap));
    const ImVec2 deckTL = ImGui::GetCursorScreenPos();
    const float wellW = (width - kGap * 2.0f) / 3.0f;
    const float wellInner = wellW - kPad * 2.0f;

    const ImGuiStyle& style = ImGui::GetStyle();
    const float fieldH = uiPx + style.FramePadding.y * 2.0f + 6.0f;
    const char* searchLegend = "Searches name, maker and description.";
    const float deckAH = kPad + legH + 8.0f + fieldH + 9.0f + tinyH + 4.0f +
                         countLineHeight() + kPad;

    const char* showNote =
        "Three states and three kinds, and every module is in exactly one of each. NOT "
        "DECLARED is not a gap in this window: the catalogue index carries no capability "
        "field, so a module's kind is only known once it is fitted.";
    // TWO COLUMNS WHEN THEY FIT, ONE WHEN THEY DO NOT. A rocker whose label
    // plate has been squeezed off the row is a switch nobody can read, so the
    // well grows taller rather than letting that happen.
    const float showColW = (wellInner - 12.0f) * 0.5f;
    const bool showTwoCols = showColW >= 74.0f;
    const float showRows = showTwoCols ? 3.0f : 6.0f;
    const float deckBH = kPad + legH + 8.0f + kRockerH * showRows + 8.0f +
                         noteHeight(wellInner, showNote) + kPad;

    const std::string sourceLine =
        model.sourceUrl.empty() ? std::string("no catalogue source set") : model.sourceUrl;
    const float srcTextW = wellInner - 92.0f - 8.0f;
    const float srcLineH = std::max(kKeyH, wrapH(uf, tiny, srcTextW, sourceLine.c_str()));
    float deckCH = kPad + legH + 8.0f + kSegH + 12.0f + 1.0f + 10.0f + legH + 8.0f +
                   srcLineH + kPad;
    if (!model.sourceStatus.empty()) {
        deckCH += 6.0f + wrapH(uf, tiny, wellInner, model.sourceStatus.c_str());
    }
    if (!model.sourceError.empty()) {
        deckCH += 6.0f + noteHeight(wellInner, model.sourceError.c_str());
    }
    if (model.busy) { deckCH += 6.0f + 14.0f + 4.0f + kKeyH; }

    const float deckH = std::max(deckAH, std::max(deckBH, deckCH));
    ImGui::Dummy(ImVec2(width, deckH));

    // ---- CATALOGUE SEARCH ---------------------------------------------------
    {
        const ImVec2 tl(deckTL.x, deckTL.y);
        const ImVec2 br(tl.x + wellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + kPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellInner, "CATALOGUE SEARCH");
        y += legH + 8.0f;

        const float clearW = 60.0f;
        const ImVec2 fTL(tl.x + kPad, y);
        const ImVec2 fBR(fTL.x + wellInner - clearW - 8.0f, y + fieldH);
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
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme::vec(theme::kInkFaint));
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
        dl->AddText(uf, tiny, ImVec2(tl.x + kPad, y), theme::kInkFaint, searchLegend,
                    nullptr, wellInner);
        y += tinyH + 4.0f;
        // WHAT IS ON SCREEN AND WHAT EXISTS, both. "3 shown" alone cannot tell
        // a short catalogue from a filter that is hiding most of it.
        drawCountLine(dl, ImVec2(tl.x + kPad, y), static_cast<int>(visible.size()),
                      static_cast<int>(model.modules.size()),
                      catState == CatalogueState::Read ? "MODULES SHOWN" : "MODULES KNOWN");
        dl->PopClipRect();
    }

    // ---- SHOW ---------------------------------------------------------------
    {
        const ImVec2 tl(deckTL.x + wellW + kGap, deckTL.y);
        const ImVec2 br(tl.x + wellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + kPad;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellInner, "SHOW");
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
        const float colW = showTwoCols ? showColW : wellInner;
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
            const float rx =
                tl.x + kPad + ((showTwoCols && i % 2 == 1) ? (colW + 12.0f) : 0.0f);
            const float ry =
                y + kRockerH * static_cast<float>(showTwoCols ? (i / 2) : i);
            char cnt[16];
            std::snprintf(cnt, sizeof cnt, "%d", rows[i].count);
            if (drawRockerRow(dl, ImVec2(rx, ry), colW, kRockerH, rows[i].label, cnt,
                              *rows[i].flag, rows[i].id)) {
                *rows[i].flag = !*rows[i].flag;
            }
        }
        y += kRockerH * showRows + 8.0f;
        drawNote(dl, ImVec2(tl.x + kPad, y), wellInner, theme::kInkMuted, showNote);
        dl->PopClipRect();
    }

    // ---- SORT and CATALOGUE SOURCE -----------------------------------------
    {
        const ImVec2 tl(deckTL.x + (wellW + kGap) * 2.0f, deckTL.y);
        const ImVec2 br(tl.x + wellW, tl.y + deckH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float y = tl.y + kPad;
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
        y += kSegH + 12.0f;
        addBenchRail(dl, tl.x + kPad, br.x - kPad, y);
        y += 10.0f;
        addBenchGroupCaption(dl, ImVec2(tl.x + kPad, y), wellInner, "CATALOGUE SOURCE");
        y += legH + 8.0f;

        // WHERE THE MODULES WOULD COME FROM, printed before the key that goes
        // and gets them. A store that will not say what it is about to contact
        // is asking for a decision it has withheld the facts for.
        dl->AddText(uf, tiny, ImVec2(tl.x + kPad, y), theme::kInkMuted,
                    sourceLine.c_str(), nullptr, srcTextW);
        if (drawDeckKey(dl, ImVec2(br.x - kPad - 92.0f, y),
                        ImVec2(br.x - kPad, y + kKeyH),
                        // AGAIN once anything has been asked, whatever came
                        // back. A failed check and an empty catalogue have
                        // both been asked, and a key still saying NOW invites
                        // the user to do again what they just did.
                        catState == CatalogueState::NeverAsked ? "CHECK NOW"
                                                               : "CHECK AGAIN",
                        nullptr,
                        !model.busy && !model.sourceUrl.empty(), "checknow")) {
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
            dl->AddText(uf, tiny, ImVec2(tl.x + kPad, y), theme::kInkFaint,
                        model.sourceStatus.c_str(), nullptr, wellInner);
            y += wrapH(uf, tiny, wellInner, model.sourceStatus.c_str());
        }
        if (!model.sourceError.empty()) {
            // VERBATIM AND IN RUST. A private repository answers 404, a TLS
            // failure says so, and the text PluginRepo wrote is the only
            // evidence the user has.
            y += 6.0f;
            drawNote(dl, ImVec2(tl.x + kPad, y), wellInner, theme::kAlarm,
                     model.sourceError.c_str());
        }
        dl->PopClipRect();
    }

    // ======================= THE BODY =======================================
    ImGui::SetCursorScreenPos(ImVec2(origin.x, deckTL.y + deckH + kGap));
    const ImVec2 bodyTL = ImGui::GetCursorScreenPos();
    const float bodyH = std::max(120.0f, origin.y + height - bodyTL.y);
    // The plate takes a third, but never at the cost of a list too narrow to
    // read a module name in - the list is what this window is FOR, and a plate
    // beside three characters of name would be the tail wagging the dog.
    const float plateW =
        std::min(std::clamp(width * 0.32f, 260.0f, 470.0f),
                 std::max(220.0f, width - kGap - 300.0f));
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
                        theme::kInkFaint, "SHOWN");
        }
        y += nameH + 6.0f;
        addBenchRail(dl, tl.x + kPad, br.x - kPad, y);
        y += 8.0f;

        const float childH = br.y - y - kPad;
        ImGui::SetCursorScreenPos(ImVec2(tl.x + kPad, y));
        ImGui::BeginChild("##modlist", ImVec2(listW - kPad * 2.0f, std::max(40.0f, childH)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const float cw = std::max(120.0f, ImGui::GetContentRegionAvail().x);
            if (visible.empty()) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                // WHY THE LIST IS EMPTY, and there are four reasons, not two.
                // "Press CHECK NOW" is the right answer to exactly one of
                // them; said to the other three it sends the user round a loop
                // that cannot end, because the check has already happened.
                const char* why = "";
                switch (catState) {
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
            constexpr float kTagW = 84.0f;
            constexpr float kActW = 92.0f;
            for (int idx : visible) {
                const StoreModule& sm = model.modules[static_cast<std::size_t>(idx)];
                const ModulePlate& p = sm.plate;
                const bool isSel = idx == deck.selected;
                const float midW = std::max(80.0f, cw - kTagW - kActW - 34.0f);
                const std::string reach = moduleReachSummary(p);

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

                const float rowsH =
                    std::max(faceH(lf, uiPx), tinyH) + 4.0f +
                    (p.blurb.empty() ? 0.0f : wrapH(uf, tiny, midW, p.blurb.c_str()) + 4.0f) +
                    tinyH + (reachBeside ? 0.0f : tinyH + 1.0f) +
                    (blockedLine.empty() ? 0.0f : noteHeight(midW, blockedLine.c_str()) + 4.0f);
                // The action column: the key, then the state word beneath it -
                // WRAPPED to the column, because "TAKES NO SIGNAL" does not fit
                // on one line there and a word running out over the card's edge
                // is worse than a word on two lines.
                const char* stateWord = moduleStateWord(p);
                const float stateWordW = std::max(40.0f, kActW - 15.0f);
                const float actH = kKeyH + 6.0f + wrapH(uf, tiny, stateWordW, stateWord);
                const float cardH = std::max(rowsH, actH) + 20.0f;

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
                    const ImVec2 tTL(cTL.x + 10.0f, cTL.y + 10.0f);
                    const ImVec2 tBR(tTL.x + kTagW, tTL.y + tinyH + 6.0f);
                    cdl->AddRectFilled(tTL, tBR, theme::kBrassBright, 1.0f);
                    addBenchBevel(cdl, tTL, tBR, 1.0f, true);
                    const char* tag = moduleKindTag(p);
                    cdl->AddText(uf, tiny,
                                 ImVec2((tTL.x + tBR.x) * 0.5f - textW(uf, tiny, tag) * 0.5f,
                                        tTL.y + 3.0f),
                                 theme::kEnamel, tag);
                }

                const float mx = cTL.x + 10.0f + kTagW + 12.0f;
                float my = cTL.y + 10.0f;
                cdl->AddText(lf, uiPx, ImVec2(mx, my),
                             isSel ? theme::kIvory : theme::kCream,
                             p.name.empty() ? "(unnamed module)" : p.name.c_str());
                float vx = mx + textW(lf, uiPx, p.name.empty() ? "(unnamed module)"
                                                               : p.name.c_str()) +
                           10.0f;
                if (!p.version.empty()) {
                    cdl->AddText(rf, tiny, ImVec2(vx, my + faceH(lf, uiPx) - faceH(rf, tiny)),
                                 theme::kAmber, p.version.c_str());
                    vx += textW(rf, tiny, p.version.c_str()) + 12.0f;
                }
                {
                    // FITTED, NOT FITTED, REFUSED - the three states in words,
                    // because "fitted" and "running" are different questions
                    // and the lamp only answers the second.
                    const char* state = !p.fitted ? "NOT FITTED"
                                        : !p.loaded ? "REFUSED"
                                                    : "FITTED";
                    const ImU32 sc = !p.fitted    ? theme::kInkFaint
                                     : !p.loaded  ? theme::kAlarm
                                                  : theme::kPhosphor;
                    cdl->AddText(uf, tiny, ImVec2(vx, my + faceH(lf, uiPx) - tinyH), sc,
                                 state);
                }
                my += std::max(faceH(lf, uiPx), tinyH) + 4.0f;
                if (!p.blurb.empty()) {
                    cdl->AddText(uf, tiny, ImVec2(mx, my), theme::kInkMuted, p.blurb.c_str(),
                                 nullptr, midW);
                    my += wrapH(uf, tiny, midW, p.blurb.c_str()) + 4.0f;
                }
                {
                    // Maker and licence on the ROW, not only on the plate: the
                    // terms a module arrives under are part of choosing it,
                    // not a detail to discover after fitting.
                    cdl->AddText(uf, tiny, ImVec2(mx, my),
                                 p.licence.empty() ? theme::kGold : theme::kInkFaint, foot);
                    if (reachBeside) {
                        cdl->AddText(uf, tiny,
                                     ImVec2(mx + textW(uf, tiny, foot) + 14.0f, my),
                                     moduleReachColour(p), reach.c_str());
                        my += tinyH;
                    } else {
                        cdl->AddText(uf, tiny, ImVec2(mx, my + tinyH + 1.0f),
                                     moduleReachColour(p), reach.c_str());
                        my += tinyH + tinyH + 1.0f;
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
                    const float ax = cBR.x - 10.0f - kActW;
                    const float ay = cTL.y + 10.0f;
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
                    const float ly = ay + kKeyH + 6.0f;
                    const ImVec2 lampC(ax + 6.0f, ly + tinyH * 0.5f);
                    drawBenchLamp(cdl, lampC, 4.5f, moduleStateColour(p),
                                  moduleStateLampLit(p), nullptr);
                    cdl->AddText(uf, tiny, ImVec2(lampC.x + 9.0f, ly), moduleStateColour(p),
                                 stateWord, nullptr, stateWordW);
                }
                ImGui::PopID();
                ImGui::SetCursorScreenPos(ImVec2(cTL.x, cBR.y + 8.0f));
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
        ImGui::BeginChild("##plate", ImVec2(plateW - kPad * 2.0f, std::max(40.0f, childH)),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        {
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const float pw = std::max(120.0f, ImGui::GetContentRegionAvail().x);
            if (deck.selected < 0 ||
                deck.selected >= static_cast<int>(model.modules.size())) {
                const char* none = "";
                switch (catState) {
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
                    if (drawDeckKey(pdl, kTL, ImVec2(kTL.x + pw - 6.0f, kTL.y + 34.0f),
                                    "FIT MODULE", nullptr, blocked.empty(), "platefit")) {
                        fitIndex_ = deck.selected;
                    }
                } else if (hasUpdate) {
                    char to[96];
                    std::snprintf(to, sizeof to, "TO v%s", sm.updateToVersion.c_str());
                    if (drawDeckKey(pdl, kTL, ImVec2(kTL.x + pw - 6.0f, kTL.y + 34.0f),
                                    "UPDATE MODULE", to, !model.busy, "plateupd")) {
                        updateIndex_ = deck.selected;
                    }
                } else {
                    drawDeckKey(pdl, kTL, ImVec2(kTL.x + pw - 6.0f, kTL.y + 34.0f),
                                "ALREADY FITTED", nullptr, false, "platefitted");
                }
                ImGui::SetCursorScreenPos(ImVec2(kTL.x, kTL.y + 34.0f + 8.0f));
                ImGui::Dummy(ImVec2(pw, 0.0f));

                float ny = kTL.y + 34.0f + 8.0f;
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
