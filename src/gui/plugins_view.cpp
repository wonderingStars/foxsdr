// plugins_view.cpp - the FITTED MODULES window. See plugins_view.hpp for what
// this window is for and why it is not the plugin store.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/plugins_view.hpp"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

// ============================================================================
// What the window SAYS. No ImGui below this line until the drawing section.
// ============================================================================

namespace {

// The three bits that make a module something PluginRunner can feed - it
// creates an instance for a decoder, an I/Q decoder or an image decoder and
// for nothing else. A module with none of them is fed nothing, by design and
// not by fault, which is the whole of the NoSignal state.
constexpr std::uint32_t kSignalCaps =
    CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;

}  // namespace

FittedState fittedState(const FittedModule& m, bool receiverRunning) {
    // REFUSED FIRST, because a refused record has no descriptor at all: its
    // name, version and capability bits are empty, so every other question
    // below would be asked of fields the host never filled in.
    if (!m.loaded) { return FittedState::Refused; }
    // STOPPED SECOND, and ahead of every "why is nothing happening" answer.
    // The user's own choice is not a fault and must not be reported as one -
    // and a stopped module is skipped before any instance is created, so the
    // runner has no opinion about it to quote.
    if (m.stopped) { return FittedState::Stopped; }
    // A module that declares no decoder is fed nothing however the receiver is
    // set. Calling that "not fed" would put a warning on a basemap doing
    // exactly what it was fitted to do.
    if ((m.capabilities & kSignalCaps) == 0u) { return FittedState::NoSignal; }
    // THE RECEIVER'S RUN STATE IS PART OF THE ANSWER. isFeeding() says the
    // runner holds an instance matched to the rate the pipeline is CONFIGURED
    // for; with the receiver stopped, that instance is handed nothing. Reading
    // it as FED would put a green lamp on a silent radio.
    if (m.fed && receiverRunning) { return FittedState::Fed; }
    return FittedState::NotFed;
}

const char* fittedStateWord(FittedState s) {
    switch (s) {
        case FittedState::Fed: return "FED";
        case FittedState::NotFed: return "NOT FED";
        case FittedState::NoSignal: return "TAKES NO SIGNAL";
        case FittedState::Stopped: return "STOPPED BY YOU";
        case FittedState::Refused: return "REFUSED";
    }
    return "UNKNOWN";
}

std::string fittedStateSentence(const FittedModule& m, bool receiverRunning) {
    switch (fittedState(m, receiverRunning)) {
        case FittedState::Refused:
            // VERBATIM. This string is the answer to "my plugin does not
            // appear" and it carries numbers a paraphrase would throw away
            // ("expected 3, plugin reports 2"). The framing that introduces it
            // is drawn separately, so nothing is added to the host's words.
            return m.error.empty()
                       ? std::string("The host refused this file and recorded no reason.")
                       : m.error;
        case FittedState::Stopped:
            return "You stopped this module. It stays installed and its code stays "
                   "mapped, and it is given no decoders, no map targets and no panels of "
                   "its own until you start it again.";
        case FittedState::NoSignal:
            return "Fitted, and it takes no signal. This module declares no decoder, so "
                   "nothing is routed to it and nothing should be - it works through the "
                   "capabilities listed above.";
        case FittedState::Fed:
            return "Fitted and being fed. The receiver is running and this module has a "
                   "decoder matched to the rate it is producing.";
        case FittedState::NotFed:
            break;
    }
    // NOT FED, which is the state with something to explain. The receiver's
    // own run state is checked FIRST because it stops every module at once,
    // and the runner's per-module sentence would be true and beside the point.
    //
    // IT STILL MATTERS WHETHER THERE IS A SECOND REASON. "Start the receiver
    // and this module is fed" is only true when the runner already holds a
    // matched instance for it; said to a module that ALSO has a rate mismatch
    // it is a promise the product cannot keep, and the user would start the
    // receiver and be back where they were with no new information.
    if (!receiverRunning) {
        if (m.fed) {
            return "Fitted, and fed nothing because the receiver is stopped. It has a "
                   "decoder matched to the rate the receiver is set to, so starting the "
                   "receiver is all this needs.";
        }
        std::string s = "Fitted, and fed nothing because the receiver is stopped.";
        if (!m.idleDetail.empty()) {
            s += " There is a second reason as well: ";
            s += m.idleDetail;
        }
        return s;
    }
    if (!m.idleDetail.empty()) {
        // The runner's own ready-to-display sentence, quoted rather than
        // rewritten: the Plugins rail prints this exact string, and one idle
        // decoder described two ways in two places is worse than one
        // description in the wrong place.
        return m.idleDetail;
    }
    return "Fitted, and not being fed. No reason was recorded for it.";
}

ModulePlate makeModulePlate(const FittedModule& m) {
    ModulePlate p;

    // WAS A DESCRIPTOR EVER READ OUT OF THIS FILE? The host copies name,
    // version, author and licence only AFTER validatePluginDesc accepts the
    // descriptor (plugin_host.cpp:232-249), and validation itself requires a
    // non-empty name - so a non-empty name here means the four fields were
    // read, and an empty one means the file was refused before anything was.
    //
    // THE TEST IS THE RECORD, NOT `loaded`. The duplicate resolver turns a
    // module off after loading it (plugin_host.cpp:826), leaving loaded false
    // on a record whose identity is entirely known; keying this off `loaded`
    // would hatch out four facts we hold.
    const bool descriptorRead = !m.name.empty();
    p.haveDescriptor = descriptorRead;

    p.name = m.loaded ? m.name : m.file;
    p.version = m.version;
    p.maker = m.author;
    p.licence = m.licence;
    p.fileName = m.file;

    // FITTED IS TRUE FOR EVERY RECORD HERE, refused ones included: a refused
    // module is a file sitting in the plugins directory, which is exactly what
    // fitted means. It is what makes the plate print the refusal reason - it
    // draws that box only for a module that is fitted and did not load.
    p.fitted = true;
    p.loaded = m.loaded;
    // The plate's own definition: loaded AND not stopped. Deliberately not
    // this window's finer FED, which also asks whether anything is reaching
    // it - the plate is a description of a module, not a meter.
    p.running = m.loaded && !m.stopped;
    p.refusalReason = m.error;

    // Only a loaded module has had its descriptor read, so only a loaded
    // module's capability word is known. A refused record's bits are 0 because
    // nothing was read, which is "not known" and not "declares nothing".
    p.haveCapabilities = m.loaded;
    p.capabilities = m.capabilities;

    // The grant is a real per-module setting whatever the module asks for, so
    // it is always looked up - but it is only meaningful for a module that can
    // ask, and the plate's own reach note says which of those two this is.
    p.haveTuneGrant = m.loaded;
    p.tuneGranted = m.tuneAllowed;

    p.haveSizeBytes = (m.sizeBytes > 0u);
    p.sizeBytes = m.sizeBytes;

    // NOT RECORDED, and said so rather than guessed. LoadedPlugin carries no
    // abiVersion: the host validates the descriptor's against
    // CASCADE_PLUGIN_ABI_VERSION at load and copies only the strings out, so
    // the number is genuinely not available from a host record. haveAbi false
    // is the plate's "not recorded", which is the same fail-open rule
    // pluginBlockReason follows and is never read as a mismatch.
    p.haveAbi = false;
    return p;
}

FittedCounts countStates(const std::vector<FittedModule>& modules, bool receiverRunning) {
    FittedCounts c;
    for (const FittedModule& m : modules) {
        ++c.total;
        // NOTHING IS ADDED TOGETHER HERE. NoSignal used to be counted into
        // notFed, and the strip lettered that total "NOT DECODING" with a lit
        // lamp over it - so a basemap, which declares no decoder and can never
        // decode anything, was reported as a decoder that is not decoding.
        // Each state counts itself and the strip letters each count with the
        // words that state actually means.
        switch (fittedState(m, receiverRunning)) {
            case FittedState::Fed: ++c.fed; break;
            case FittedState::NotFed: ++c.notFed; break;
            case FittedState::NoSignal: ++c.noSignal; break;
            case FittedState::Stopped: ++c.stopped; break;
            case FittedState::Refused: ++c.refused; break;
        }
    }
    return c;
}

FittedModule makeFittedModule(const cascade::core::LoadedPlugin& p, bool stopped, bool fed,
                              std::string idleDetail, bool tuneAllowed) {
    FittedModule m;
    m.file = cascade::core::pluginKey(p);
    m.path = p.path;
    m.name = p.name;
    m.version = p.version;
    m.author = p.author;
    m.licence = p.licence;
    m.capabilities = p.capabilities;
    m.loaded = p.loaded;
    m.error = p.error;
    m.stopped = stopped;
    m.fed = fed;
    m.idleDetail = std::move(idleDetail);
    // From the TABLE POINTER and not from the capability bit. The host clears
    // a table it could not accept, so a module that declared the bit and
    // supplied nothing usable would otherwise be offered a grant it cannot use.
    m.tuneCapable = (p.hostClient != nullptr);
    m.tuneAllowed = tuneAllowed;
    return m;
}

// ============================================================================
// The bench vocabulary this window adds.
//
// scope_face.hpp holds the shared primitives - bevels, rails, dividers,
// plates, group captions, lamps, keys - and every one that fits is used below.
// What is added here is what that header does not have and this panel needs: a
// recessed WELL, a LABELLED brass key (drawBenchKey is a square with no
// lettering), a small toggle key with its own lamp, and a NOTE. They are
// local for the reason map_view.cpp gives for its own copies: that header is
// not this agent's to change, and promoting them is a change to make once,
// deliberately, when a third window wants them - not a change to make by
// editing a shared header from inside a feature.
// ============================================================================

namespace {

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

float faceH(ImFont* f, float px) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y; }

// A figure is a figure only if it is made of figures. fonts.hpp is narrow
// about the monospaced face for a measured reason - Nova Mono's capitals merge
// into blocks below about 20 px - so it is asked for by TESTING the string
// rather than by a call site's opinion of what it holds.
bool figureLike(const char* s) {
    if (s == nullptr || s[0] == '\0') { return false; }
    for (const char* p = s; *p != '\0'; ++p) {
        const bool ok = (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '+' ||
                        *p == ':' || *p == ' ';
        if (!ok) { return false; }
    }
    return true;
}

ImFont* faceForValue(const char* s) {
    return figureLike(s) ? cascade::gui::fonts::reading() : cascade::gui::fonts::ui();
}

// The recessed bay a group of controls sits in: dark enamel cut into the
// panel, a brass lip around it and the bevel lit from below, which is what
// makes it read as a hole rather than as a dark rectangle.
void addDeckWell(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    if (dl == nullptr || br.x - tl.x < 8.0f || br.y - tl.y < 8.0f) { return; }
    const float r = theme::kPanelRounding;
    dl->AddRectFilled(tl, br, theme::kEnamelDark, r);
    // AddRectFilledMultiColor cannot round its corners, so the shape is laid
    // flat first and the gradient inset by the radius - the same trick
    // addBenchPlate uses.
    if (br.x - tl.x > r * 2.0f) {
        dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y),
                                    theme::kEnamelDark, theme::kEnamelDark, theme::kWell,
                                    theme::kWell);
    }
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassBright, 0.75f), r, 0, 2.0f);
    addBenchBevel(dl, tl, br, r, false);
}

// A labelled brass key. Disabled draws it drained and refuses the click, which
// is what a control that cannot do its job should look like - the sentence
// saying WHY is always beside it, because a greyed key with no explanation is
// the fault this window exists to remove.
bool drawDeckKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                 bool enabled, const char* id) {
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

    // ENGRAVED INTO BRASS, which the design's own rule allows for a caption on
    // metal and forbids for a reading on glass. A dead key letters in the
    // faint ink instead, so it reads as unavailable rather than as unlabelled.
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const ImU32 ink = enabled ? theme::kEnamel : theme::kInkFaint;
    dl->AddText(f, px,
                ImVec2((tl.x + br.x) * 0.5f - textW(f, px, label) * 0.5f,
                       (tl.y + br.y) * 0.5f - faceH(f, px) * 0.5f + (held ? 1.0f : 0.0f)),
                ink, label);
    return pressed;
}

// A latching filter key: the word, and a lamp that says whether the state it
// names is being shown. Pressed in when on, proud when off, so the setting is
// legible before any colour is read.
bool drawFilterKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                   bool on, ImU32 lamp, const char* id) {
    if (dl == nullptr || br.x - tl.x < 8.0f) { return false; }
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##filter", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const float r = theme::kKeyRounding;
    if (on) {
        dl->AddRectFilled(tl, br, theme::kBrassBright, r);
        dl->AddRectFilledMultiColor(ImVec2(tl.x + r, tl.y), ImVec2(br.x - r, br.y),
                                    hovered ? theme::kIvory : theme::kCream,
                                    hovered ? theme::kIvory : theme::kCream,
                                    theme::kBrassBright, theme::kBrassBright);
        addBenchBevel(dl, tl, br, r, false);
    } else {
        dl->AddRectFilled(tl, br, theme::kEnamel, r);
        dl->AddRect(tl, br, theme::kBrassDark, r, 0, theme::kHairline);
        addBenchBevel(dl, tl, br, r, true);
    }

    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float lampR = 3.5f;
    const float lx = tl.x + 9.0f;
    drawBenchLamp(dl, ImVec2(lx, (tl.y + br.y) * 0.5f), lampR, lamp, on, nullptr);
    // CLIPPED TO THE KEY. The user can make this window narrow enough that a
    // word does not fit its key, and a legend running out across the panel is
    // worse than one that is cut - the lamp beside it still says the state.
    dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y), ImVec2(br.x - 2.0f, br.y), true);
    dl->AddText(f, px, ImVec2(lx + lampR + 6.0f, (tl.y + br.y) * 0.5f - faceH(f, px) * 0.5f),
                on ? theme::kEnamel : theme::kInkMuted, label);
    dl->PopClipRect();
    return pressed;
}

float noteHeight(float width, const char* text) {
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float wrap = width - 12.0f;
    if (wrap < 20.0f) { return faceH(f, px) + 8.0f; }
    return f->CalcTextSizeA(px, FLT_MAX, wrap, text).y + 8.0f;
}

void drawNote(ImDrawList* dl, const ImVec2& tl, float width, ImU32 accent,
              const char* text) {
    if (dl == nullptr || width < 30.0f) { return; }
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float h = noteHeight(width, text);
    dl->AddRectFilled(tl, ImVec2(tl.x + width, tl.y + h), theme::withAlpha(accent, 0.10f));
    dl->AddRectFilled(tl, ImVec2(tl.x + 2.0f, tl.y + h), accent);
    dl->AddText(f, px, ImVec2(tl.x + 9.0f, tl.y + 4.0f), accent, text, nullptr, width - 12.0f);
}

// The lamp colour for a state.
//
// AMBER IS NOT USED HERE. theme::warning() is kAmber, and the palette's own
// rule is that amber is a READING - the counts on the strip above are amber
// because they are measurements. A state that wants looking at takes kGold,
// which is the tone the design's own held-updates lamp uses, so a lamp and a
// number can never be mistaken for one another.
ImU32 stateLamp(FittedState s) {
    switch (s) {
        case FittedState::Fed: return theme::kPhosphor;
        case FittedState::NotFed: return theme::kGold;
        case FittedState::NoSignal: return theme::kBrassTint;
        case FittedState::Stopped: return theme::kBrassTint;
        case FittedState::Refused: return theme::kAlarm;
    }
    return theme::kBrassTint;
}

// The ink the state WORD is lettered in. Deliberately not the lamp colour for
// every state: a stopped module is a choice the user made, so it letters in
// plain ivory rather than in anything that reads as trouble.
ImU32 stateInk(FittedState s) {
    switch (s) {
        case FittedState::Fed: return theme::kPhosphor;
        case FittedState::NotFed: return theme::kGold;
        case FittedState::NoSignal: return theme::kInkMuted;
        case FittedState::Stopped: return theme::kCream;
        case FittedState::Refused: return theme::kAlarmHot;
    }
    return theme::kInkMuted;
}

// Whether a lamp is LIT. Only a module actually being fed lights one: a panel
// of lit lamps means nothing, and the whole point of this window is that a
// green lamp is worth something.
bool stateLampLit(FittedState s) { return s == FittedState::Fed || s == FittedState::Refused; }

// One row's height, measured from the text that will actually be in it. A row
// sized from a different string to the one drawn is a row that clips itself.
float rowHeight(const std::string& body, float bodyWidth) {
    ImFont* uf = cascade::gui::fonts::ui();
    const float titleH = faceH(uf, cascade::gui::fonts::kUiSize);
    const float tinyH = faceH(uf, cascade::gui::fonts::kTinySize);
    const float wrap = std::max(40.0f, bodyWidth);
    const float bodyH =
        uf->CalcTextSizeA(cascade::gui::fonts::kTinySize, FLT_MAX, wrap, body.c_str()).y;
    // The floor is what the right-hand column needs: the state word on the
    // title line and the START/STOP key beneath it.
    const float needed = 9.0f + titleH + 4.0f + bodyH + 3.0f + tinyH + 9.0f;
    return std::max(needed, 9.0f + titleH + 6.0f + 24.0f + 9.0f);
}

}  // namespace

// ============================================================================
// The window.
// ============================================================================

namespace {

// ---- WHAT THIS WINDOW ADDS TO THE SHARED PLATE ------------------------------
//
// The data plate itself is drawModuleDataPlate() in plugin_store_view.hpp: the
// identity, the facts, the reach panel with its corrected claim, and the
// refusal reason. It is a DESCRIPTION of a module and is deliberately the same
// in both windows.
//
// This well is the part that is only true on this machine at this moment, and
// therefore belongs to the operating panel rather than to the plate:
//
//   WHAT IT IS DOING - the state word and the sentence behind it. The plate
//   cannot know this; it is handed no receiver, no runner and no stop set.
//
//   LOADED FROM - the absolute path. A module's file name embeds its version,
//   so installing 1.1.0 over 1.0.1 ADDS a file rather than replacing one, and
//   the host's duplicate resolver turns the loser off with a reason. "Which
//   copy is running" had no answer anywhere in this product before this line.
//
// Returns the y below the well.
float drawOperatingWell(ImDrawList* dl, float x, float y, float width,
                        const FittedModule& m, bool receiverRunning) {
    ImFont* uf = cascade::gui::fonts::ui();
    const float tiny = cascade::gui::fonts::kTinySize;
    // MEASURED IN THE FACE IT IS DRAWN IN. addBenchGroupCaption letters in the
    // LEGEND face; advancing by the ui face's height would leave the caption
    // and what follows it a pixel out at every size.
    const float capH = faceH(cascade::gui::fonts::legend(), tiny);
    const float pad = 12.0f;
    const float inner = width - pad * 2.0f;
    if (inner < 60.0f) { return y; }

    const FittedState st = fittedState(m, receiverRunning);
    const std::string sentence = fittedStateSentence(m, receiverRunning);
    const std::string path = m.path.empty() ? std::string("not recorded") : m.path;
    const float pathH = uf->CalcTextSizeA(tiny, FLT_MAX, inner, path.c_str()).y;

    const float wellH = pad + capH + 6.0f + noteHeight(inner, sentence.c_str()) + 12.0f +
                        capH + 4.0f + pathH + pad;
    addDeckWell(dl, ImVec2(x, y), ImVec2(x + width, y + wellH));
    dl->PushClipRect(ImVec2(x + 2.0f, y + 2.0f), ImVec2(x + width - 2.0f, y + wellH - 2.0f),
                     true);

    float ty = y + pad;
    // THE CAPTION CARRIES THE STATE WORD, so the well says which of the five
    // answers this is before the sentence is read - and so a greyscale
    // screenshot still says it, which a coloured note alone would not.
    char caption[64];
    std::snprintf(caption, sizeof caption, "WHAT IT IS DOING - %s", fittedStateWord(st));
    addBenchGroupCaption(dl, ImVec2(x + pad, ty), inner, caption);
    ty += capH + 6.0f;
    drawNote(dl, ImVec2(x + pad, ty), inner, stateInk(st), sentence.c_str());
    ty += noteHeight(inner, sentence.c_str()) + 12.0f;

    addBenchGroupCaption(dl, ImVec2(x + pad, ty), inner, "LOADED FROM");
    ty += capH + 4.0f;
    dl->AddText(uf, tiny, ImVec2(x + pad, ty),
                m.path.empty() ? theme::kInkFaint : theme::kInkMuted, path.c_str(), nullptr,
                inner);

    dl->PopClipRect();
    return y + wellH;
}

}  // namespace

FittedModulesAction drawFittedModulesPanel(FittedModulesDeck& deck,
                                           const FittedModulesModel& model) {
    FittedModulesAction act;

    ImGui::PushID("fittedmodules");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 240.0f || avail.y < 160.0f) {
        // Too small to letter honestly. Say why rather than drawing a clipped
        // panel that looks broken.
        ImGui::TextDisabled("Too narrow to draw. Widen the window.");
        ImGui::PopID();
        return act;
    }

    ImFont* uf = cascade::gui::fonts::ui();
    const float tiny = cascade::gui::fonts::kTinySize;
    const float uiPx = cascade::gui::fonts::kUiSize;
    const float readPx = cascade::gui::fonts::kReadingSize;
    const float tinyH = faceH(uf, tiny);
    // The engraved captions letter in the LEGEND face, so their line advance
    // is measured in that face and not in the ui one.
    const float capH = faceH(cascade::gui::fonts::legend(), tiny);
    const float readH = faceH(cascade::gui::fonts::reading(), readPx);

    const FittedCounts counts = countStates(model.modules, model.receiverRunning);

    // ======================= THE PLATE ======================================
    const float titlePlateH = 40.0f;
    addBenchPlate(dl, origin, ImVec2(origin.x + avail.x, origin.y + titlePlateH),
                  "FITTED MODULES");
    float y = origin.y + titlePlateH + 10.0f;

    // ======================= THE STRIP ======================================
    //
    // FOUR COUNTS AND THE RECEIVER, which between them are the whole answer to
    // "is anything working" without opening a single row.
    const char* rxNote =
        model.receiverRunning
            ? "The receiver is running, so a module with a matched decoder is being fed."
            : "The receiver is stopped, so NOTHING is being fed to any module however it "
              "is set. That is why no module below reads FED.";
    const float stripInner = avail.x - 24.0f;
    const float rescanW = 96.0f;
    const float stripNoteW = stripInner;
    constexpr int kGroups = 5;
    const float groupW = stripInner / static_cast<float>(kGroups);
    // The five counts. AMBER AND THE MONOSPACED FACE, because each is a
    // measurement of what is on this machine - the palette's rule for a
    // reading, and the one place on this panel amber is correct.
    //
    // FIVE, NOT FOUR, and this is the fix for a caption that lied. NOT
    // DECODING used to carry NoSignal as well, so a basemap counted as a
    // decoder that was not decoding and lit a gold lamp for it. A module that
    // takes no signal now has its own count, its own words and no lamp at all:
    // it is doing exactly what it was fitted to do.
    struct Group {
        int n;
        const char* word;
        ImU32 lamp;
        bool lit;
    };
    const Group groups[kGroups] = {
        {counts.fed, "FED", theme::kPhosphor, counts.fed > 0},
        {counts.notFed, "NOT DECODING", theme::kGold, counts.notFed > 0},
        {counts.noSignal, "TAKES NO SIGNAL", theme::kBrassTint, false},
        {counts.stopped, "STOPPED", theme::kBrassTint, false},
        {counts.refused, "REFUSED", theme::kAlarm, counts.refused > 0},
    };
    // THE WORDS ARE WRAPPED INSIDE THEIR OWN GROUP AND THE ROW IS SIZED FROM
    // THE TALLEST. The user can narrow this window until "NOT DECODING" needs
    // two lines, and a strip measured from one line would then print the
    // second through the sentence beneath it.
    const float wordWrap = std::max(30.0f, groupW - 24.0f);
    float wordH = 0.0f;
    for (const Group& g : groups) {
        wordH = std::max(wordH, uf->CalcTextSizeA(tiny, FLT_MAX, wordWrap, g.word).y);
    }
    const float stripH = 12.0f + capH + 8.0f + readH + 2.0f + wordH + 10.0f +
                         noteHeight(stripNoteW, rxNote) + 12.0f;
    {
        const ImVec2 tl(origin.x, y);
        const ImVec2 br(origin.x + avail.x, y + stripH);
        addDeckWell(dl, tl, br);
        dl->PushClipRect(ImVec2(tl.x + 2.0f, tl.y + 2.0f), ImVec2(br.x - 2.0f, br.y - 2.0f),
                         true);
        float ty = tl.y + 12.0f;
        addBenchGroupCaption(dl, ImVec2(tl.x + 12.0f, ty), stripInner - rescanW - 10.0f,
                             "WHAT IS FITTED");

        // SCAN AGAIN, on the strip because it is the one action that is about
        // the whole folder rather than about one module.
        const ImVec2 rtl(br.x - 12.0f - rescanW, ty - 4.0f);
        if (drawDeckKey(dl, rtl, ImVec2(rtl.x + rescanW, rtl.y + 24.0f), "SCAN AGAIN", true,
                        "rescan")) {
            act.kind = FittedModulesAction::Kind::Rescan;
        }
        ty += capH + 8.0f;

        for (int i = 0; i < kGroups; ++i) {
            const float gx = tl.x + 12.0f + groupW * static_cast<float>(i);
            if (i > 0) { addBenchDivider(dl, gx - 6.0f, ty - 2.0f, ty + readH + wordH + 4.0f); }
            drawBenchLamp(dl, ImVec2(gx + 6.0f, ty + readH * 0.5f), 5.0f, groups[i].lamp,
                          groups[i].lit, nullptr);
            char num[16];
            std::snprintf(num, sizeof num, "%d", groups[i].n);
            dl->AddText(cascade::gui::fonts::reading(), readPx, ImVec2(gx + 18.0f, ty),
                        groups[i].n > 0 ? theme::kAmber : theme::kAmberDim, num);
            dl->AddText(uf, tiny, ImVec2(gx + 18.0f, ty + readH + 2.0f), theme::kInkMuted,
                        groups[i].word, nullptr, wordWrap);
        }
        ty += readH + 2.0f + wordH + 10.0f;

        drawNote(dl, ImVec2(tl.x + 12.0f, ty), stripNoteW,
                 model.receiverRunning ? theme::kPhosphor : theme::kGold, rxNote);
        dl->PopClipRect();
    }
    y += stripH + 10.0f;

    // ======================= THE FOLDER LINE ================================
    {
        // WHERE THE SCAN LOOKED, on its own line and never truncated to
        // nothing: "my plugin does not appear" is sometimes answered by the
        // module having been dropped into the other one of the two directories
        // PluginHost chooses between.
        const std::string dir =
            model.directory.empty() ? std::string("No directory has been scanned yet.")
                                    : model.directory;
        addBenchGroupCaption(dl, ImVec2(origin.x + 2.0f, y), avail.x - 4.0f,
                             "MODULES ARE READ FROM");
        y += capH + 3.0f;
        // WRAPPED, not truncated. An installed copy under Program Files puts
        // the modules under %LOCALAPPDATA% instead (see
        // PluginHost::defaultPluginDir), and those paths are long enough that
        // a line cut at the panel edge would hide the very part that says
        // WHICH of the two directories this is.
        const float dirH = uf->CalcTextSizeA(tiny, FLT_MAX, avail.x - 4.0f, dir.c_str()).y;
        dl->AddText(uf, tiny, ImVec2(origin.x + 2.0f, y),
                    model.directory.empty() ? theme::kInkFaint : theme::kInkMuted,
                    dir.c_str(), nullptr, avail.x - 4.0f);
        y += dirH + 8.0f;
    }

    // Whatever the last install or remove said, verbatim and in its own
    // colour, because a Remove that FAILED is exactly when the user needs
    // telling and the row it acted on has already gone.
    if (!model.error.empty()) {
        const float h = noteHeight(avail.x, model.error.c_str());
        drawNote(dl, ImVec2(origin.x, y), avail.x, theme::kAlarm, model.error.c_str());
        y += h + 8.0f;
    } else if (!model.report.empty()) {
        const float h = noteHeight(avail.x, model.report.c_str());
        drawNote(dl, ImVec2(origin.x, y), avail.x, theme::kPhosphor, model.report.c_str());
        y += h + 8.0f;
    }

    // ======================= THE FILTER KEYS ================================
    {
        const float keyH = 24.0f;
        const float gap = 8.0f;
        constexpr int kFilters = 5;
        const float keyW =
            std::min(150.0f, (avail.x - gap * static_cast<float>(kFilters - 1)) /
                                 static_cast<float>(kFilters));
        struct Filter {
            const char* label;
            bool* flag;
            ImU32 lamp;
        };
        // ONE KEY PER STATE, and the same five as the counts above. The key
        // labelled NOT DECODING used to hide the modules that take no signal
        // as well, which is a key that does not do what it says: a basemap is
        // not a decoder that has stopped decoding.
        const Filter filters[kFilters] = {
            {"FED", &deck.showFed, theme::kPhosphor},
            {"NOT DECODING", &deck.showIdle, theme::kGold},
            {"TAKES NO SIGNAL", &deck.showNoSignal, theme::kBrassTint},
            {"STOPPED", &deck.showStopped, theme::kBrassTint},
            {"REFUSED", &deck.showRefused, theme::kAlarm},
        };
        for (int i = 0; i < kFilters; ++i) {
            const ImVec2 tl(origin.x + (keyW + gap) * static_cast<float>(i), y);
            char id[24];
            std::snprintf(id, sizeof id, "filter%d", i);
            if (drawFilterKey(dl, tl, ImVec2(tl.x + keyW, tl.y + keyH), filters[i].label,
                              *filters[i].flag, filters[i].lamp, id)) {
                *filters[i].flag = !*filters[i].flag;
            }
        }
        y += keyH + 10.0f;
    }

    // ======================= THE TWO COLUMNS ================================
    const float remainingH = (origin.y + avail.y) - y;
    if (remainingH < 80.0f) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::Dummy(ImVec2(avail.x, 0.0f));
        ImGui::PopID();
        return act;
    }

    // Which modules the filters leave. Built as indices so the deck's
    // selection means a position in the list the user is actually looking at.
    std::vector<int> visible;
    visible.reserve(model.modules.size());
    for (std::size_t i = 0; i < model.modules.size(); ++i) {
        const FittedState st = fittedState(model.modules[i], model.receiverRunning);
        bool show = true;
        switch (st) {
            case FittedState::Fed: show = deck.showFed; break;
            case FittedState::NotFed: show = deck.showIdle; break;
            case FittedState::NoSignal: show = deck.showNoSignal; break;
            case FittedState::Stopped: show = deck.showStopped; break;
            case FittedState::Refused: show = deck.showRefused; break;
        }
        if (show) { visible.push_back(static_cast<int>(i)); }
    }
    if (deck.selected < 0) { deck.selected = 0; }
    if (deck.selected >= static_cast<int>(visible.size())) {
        deck.selected = visible.empty() ? 0 : static_cast<int>(visible.size()) - 1;
    }

    // NARROW WINDOWS STACK. A 430 px plate beside a list needs about 800 px to
    // be two readable columns; below that the plate goes under the list rather
    // than both being squeezed into unreadable ones.
    const bool narrow = avail.x < 760.0f;
    const float gap = 10.0f;
    const float plateW = narrow ? avail.x : std::clamp(avail.x * 0.42f, 330.0f, 460.0f);
    const float listW = narrow ? avail.x : (avail.x - plateW - gap);
    const float listH = narrow ? std::max(120.0f, remainingH * 0.45f) : remainingH;
    const float plateColH = narrow ? std::max(120.0f, remainingH - listH - gap) : remainingH;

    // ---- the list ----------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    if (ImGui::BeginChild("##modlist", ImVec2(listW, listH), 0,
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const ImVec2 lo = ImGui::GetCursorScreenPos();
        const float innerW = ImGui::GetContentRegionAvail().x;

        if (visible.empty()) {
            // WHAT AN EMPTY MODEL PROVES, AND WHAT IT DOES NOT. The host's
            // scan only ever collects files with the platform's module
            // extension, so an empty list means nothing in that folder was
            // OFFERED to the loader - it does not mean the folder is empty. A
            // module the version policy retires is renamed aside precisely so
            // the scan stops seeing it, and it is still a file sitting there;
            // "no module file was found" told that user their folder was bare.
            const char* why =
                model.modules.empty()
                    ? "No module the host can load was found in the folder above, so "
                      "nothing is fitted. A module the version policy has retired is "
                      "renamed aside and stops being scanned - it is still a file in "
                      "that folder, and the rail's Plugins section lists it under "
                      "Disabled."
                    : "Every fitted module is hidden by the keys above. Press one to show "
                      "it again.";
            drawNote(ldl, lo, innerW, theme::kBrassShade, why);
            ImGui::SetCursorScreenPos(ImVec2(lo.x, lo.y + noteHeight(innerW, why)));
            ImGui::Dummy(ImVec2(innerW, 0.0f));
        } else {
            const float tagW = 82.0f;
            const float keyW = 78.0f;
            const float bodyX = tagW + 12.0f;
            const float bodyW = innerW - bodyX - keyW - 20.0f;
            float ry = lo.y;
            for (std::size_t vi = 0; vi < visible.size(); ++vi) {
                const FittedModule& m = model.modules[static_cast<std::size_t>(visible[vi])];
                const FittedState st = fittedState(m, model.receiverRunning);
                const bool selected = (static_cast<int>(vi) == deck.selected);
                // THE ROW'S TAG AND ITS ONE-LINE REACH COME FROM THE SHARED
                // COMPONENT, called on the same ModulePlate the plate below
                // draws. A module that read "DECODER" in the store and
                // something else here would be one module described two ways.
                const ModulePlate rowPlate = makeModulePlate(m);
                // A REFUSED MODULE'S LINE IS ITS REFUSAL. Nothing was read out
                // of the file, so it has no reach to summarise, and the reason
                // it did not load is the only thing worth the space.
                const std::string body = (st == FittedState::Refused)
                                             ? fittedStateSentence(m, model.receiverRunning)
                                             : moduleReachSummary(rowPlate);
                const float h = rowHeight(body, bodyW);
                const ImVec2 tl(lo.x, ry);
                const ImVec2 br(lo.x + innerW, ry + h);

                // KEYED ON THE FILE NAME, which is unique across a scan (one
                // record per file in one directory) and stable when the
                // filters reorder the list. A record with no path yields an
                // empty key, and an empty id would make two such rows one
                // widget, so those fall back to the position.
                if (m.file.empty()) {
                    ImGui::PushID(static_cast<int>(vi));
                } else {
                    ImGui::PushID(m.file.c_str());
                }

                // THE ROW IS THE SELECTOR, with the stop key allowed to
                // overlap it - the key is submitted afterwards and therefore
                // wins the pointer where the two meet.
                ImGui::SetCursorScreenPos(tl);
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##row", ImVec2(innerW, h))) {
                    deck.selected = static_cast<int>(vi);
                }
                const bool hovered = ImGui::IsItemHovered();

                ldl->AddRectFilled(tl, br, selected ? theme::kEnamel : theme::kWell,
                                   theme::kKeyRounding);
                if (hovered && !selected) {
                    ldl->AddRectFilled(tl, br, theme::withAlpha(theme::kBrassMid, 0.18f),
                                       theme::kKeyRounding);
                }
                ldl->AddRect(tl, br,
                             selected ? theme::kBrassBright : theme::kBrassDark,
                             theme::kKeyRounding, 0, theme::kHairline);
                // The selected row is marked in rust down its left edge, the
                // way the design marks it, so selection survives a greyscale
                // screenshot.
                if (selected) {
                    ldl->AddRectFilled(tl, ImVec2(tl.x + 3.0f, br.y), theme::kAlarm);
                }

                // The kind tag: a brass chip, engraved.
                const ImVec2 ttl(tl.x + 8.0f, tl.y + 9.0f);
                const ImVec2 tbr(ttl.x + tagW - 8.0f, ttl.y + tinyH + 6.0f);
                ldl->AddRectFilled(ttl, tbr, theme::kBrassBright, theme::kKeyRounding);
                addBenchBevel(ldl, ttl, tbr, theme::kKeyRounding, true);
                const char* tag = moduleKindTag(rowPlate);
                ldl->AddText(uf, tiny,
                             ImVec2((ttl.x + tbr.x) * 0.5f - textW(uf, tiny, tag) * 0.5f,
                                    ttl.y + 3.0f),
                             theme::kEnamel, tag);

                // The lamp and the state word sit at the right of the title
                // line, so the title is CLIPPED to what is left rather than
                // being allowed to run underneath them. A name long enough to
                // reach the word would otherwise print through it and both
                // would be unreadable.
                const char* word = fittedStateWord(st);
                const float wordW = textW(uf, tiny, word);
                const float lampX = br.x - keyW - 18.0f;
                const float titleRight =
                    std::max(tl.x + bodyX + 40.0f, lampX - 10.0f - wordW - 10.0f);

                float ty = tl.y + 9.0f;
                const char* shown = m.loaded ? m.name.c_str() : m.file.c_str();
                ldl->PushClipRect(ImVec2(tl.x + bodyX, tl.y), ImVec2(titleRight, br.y), true);
                ldl->AddText(uf, uiPx, ImVec2(tl.x + bodyX, ty),
                             selected ? theme::kIvory : theme::kCream, shown);
                const float nameW = textW(uf, uiPx, shown);
                if (!m.version.empty()) {
                    // The version takes the monospaced face and stays in ink:
                    // it identifies a build, and amber on this panel is
                    // reserved for a measurement.
                    ldl->AddText(faceForValue(m.version.c_str()),
                                 figureLike(m.version.c_str()) ? readPx : tiny,
                                 ImVec2(tl.x + bodyX + nameW + 10.0f, ty + 2.0f),
                                 theme::kInkMuted, m.version.c_str());
                }
                ldl->PopClipRect();
                ty += faceH(uf, uiPx) + 4.0f;

                const float bodyH =
                    uf->CalcTextSizeA(tiny, FLT_MAX, std::max(40.0f, bodyW), body.c_str()).y;
                ldl->AddText(uf, tiny, ImVec2(tl.x + bodyX, ty),
                             st == FittedState::Refused ? theme::kAlarmHot
                                                        : moduleReachColour(rowPlate),
                             body.c_str(), nullptr, std::max(40.0f, bodyW));
                ty += bodyH + 3.0f;
                ldl->AddText(uf, tiny, ImVec2(tl.x + bodyX, ty), theme::kInkFaint,
                             m.file.c_str(), nullptr, std::max(40.0f, bodyW));

                // The lamp and the state word, right of the name. Both are
                // drawn whatever the colour says: a lamp whose meaning is
                // carried by colour alone cannot be read in a greyscale
                // screenshot and is unreadable to about one man in twelve.
                drawBenchLamp(ldl, ImVec2(lampX, tl.y + 9.0f + faceH(uf, uiPx) * 0.5f), 4.5f,
                              stateLamp(st), stateLampLit(st), nullptr);
                ldl->AddText(uf, tiny,
                             ImVec2(lampX - 10.0f - wordW,
                                    tl.y + 9.0f + faceH(uf, uiPx) * 0.5f - tinyH * 0.5f),
                             stateInk(st), word);

                // START / STOP on the row, because it is the action a user
                // takes over and over and the plate is one selection away.
                // THE LABEL IS THE ACTION, never the state: a key saying
                // "RUNNING" leaves the user guessing whether pressing it stops
                // the module or is simply a badge.
                if (m.loaded) {
                    const ImVec2 ktl(br.x - keyW - 8.0f, tl.y + 9.0f + faceH(uf, uiPx) + 6.0f);
                    if (drawDeckKey(ldl, ktl, ImVec2(ktl.x + keyW, ktl.y + 24.0f),
                                    m.stopped ? "START" : "STOP", true, "rowstop")) {
                        act.kind = m.stopped ? FittedModulesAction::Kind::Start
                                             : FittedModulesAction::Kind::Stop;
                        act.file = m.file;
                        deck.selected = static_cast<int>(vi);
                    }
                }

                ImGui::PopID();
                ry += h + 6.0f;
            }
            ImGui::SetCursorScreenPos(ImVec2(lo.x, ry));
            ImGui::Dummy(ImVec2(innerW, 0.0f));
        }
    }
    ImGui::EndChild();

    // ---- the plate ---------------------------------------------------------
    const ImVec2 plateOrigin = narrow ? ImVec2(origin.x, y + listH + gap)
                                      : ImVec2(origin.x + listW + gap, y);
    ImGui::SetCursorScreenPos(plateOrigin);
    if (ImGui::BeginChild("##modplate", ImVec2(plateW, plateColH), 0,
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        const ImVec2 po = ImGui::GetCursorScreenPos();
        const float innerW = ImGui::GetContentRegionAvail().x;

        if (visible.empty()) {
            drawNote(pdl, po, innerW, theme::kBrassShade,
                     "Select a module to see its plate.");
            ImGui::SetCursorScreenPos(
                ImVec2(po.x, po.y + noteHeight(innerW, "Select a module to see its plate.")));
            ImGui::Dummy(ImVec2(innerW, 0.0f));
        } else {
            const FittedModule& m =
                model.modules[static_cast<std::size_t>(visible[static_cast<std::size_t>(
                    deck.selected)])];
            // AN ARMED DELETE FOLLOWS ITS MODULE AND NOTHING ELSE. Selecting
            // another row disarms it, so a confirm cannot be inherited by a
            // module the user merely clicked on next.
            if (!deck.confirmRemove.empty() && deck.confirmRemove != m.file) {
                deck.confirmRemove.clear();
            }
            ImGui::PushID("plate");
            // THE SHARED PLATE, unchanged and unwrapped: the identity, the
            // facts, the reach panel with its corrected claim and the refusal
            // reason, drawn by the store's component so this module reads
            // identically in both windows.
            const ModulePlate plate = makeModulePlate(m);
            float py = po.y + drawModuleDataPlate(pdl, po, innerW, plate) + 10.0f;
            // Then what only this window can say: the operating state, and the
            // path the module was loaded from.
            py = drawOperatingWell(pdl, po.x, py, innerW, m, model.receiverRunning) + 10.0f;

            // ---- the actions ---------------------------------------------
            const float keyH = 32.0f;
            const float half = (innerW - 8.0f) * 0.5f;
            if (m.loaded) {
                if (drawDeckKey(pdl, ImVec2(po.x, py), ImVec2(po.x + half, py + keyH),
                                m.stopped ? "START MODULE" : "STOP MODULE", true,
                                "platestop")) {
                    act.kind = m.stopped ? FittedModulesAction::Kind::Start
                                         : FittedModulesAction::Kind::Stop;
                    act.file = m.file;
                }
            } else {
                // A refused file has no instances to start. A key here would
                // imply the reason it is silent is something the user did.
                drawDeckKey(pdl, ImVec2(po.x, py), ImVec2(po.x + half, py + keyH),
                            "NOTHING TO START", false, "platestopdead");
            }

            // TWO-STEP REMOVE. Deleting a module deletes a file the user
            // downloaded and may not be able to get back, so a single mis-click
            // must not do it.
            const bool armed = (deck.confirmRemove == m.file);
            if (drawDeckKey(pdl, ImVec2(po.x + half + 8.0f, py), ImVec2(po.x + innerW, py + keyH),
                            armed ? "CONFIRM DELETE" : "REMOVE MODULE", true,
                            "plateremove")) {
                if (armed) {
                    act.kind = FittedModulesAction::Kind::Remove;
                    act.file = m.file;
                    deck.confirmRemove.clear();
                } else {
                    deck.confirmRemove = m.file;
                }
            }
            py += keyH + 8.0f;

            if (armed) {
                static const char* kArmed =
                    "Press CONFIRM DELETE again to delete this file from the modules "
                    "folder. Selecting another module cancels it.";
                drawNote(pdl, ImVec2(po.x, py), innerW, theme::kAlarm, kArmed);
                py += noteHeight(innerW, kArmed) + 8.0f;
            }

            // THE ONE REACH THAT IS ENFORCED, and therefore the one that gets a
            // key. The plate above states in words that a fitted module runs
            // with the application's own privileges and that nothing else on
            // its list is a gate; this is the exception it names. The grant is
            // per module, it defaults to off, and without it PluginUi answers
            // every request_tune with CASCADE_TUNE_DENIED.
            //
            // The key is offered only for a module that ASKS - one that
            // declares no host-client table can never use the grant, and a
            // control that changes a setting nothing reads is a control that
            // lies about having done something.
            if (m.loaded && m.tuneCapable) {
                if (drawDeckKey(pdl, ImVec2(po.x, py), ImVec2(po.x + innerW, py + keyH),
                                m.tuneAllowed ? "REVOKE RECEIVER CONTROL"
                                              : "GRANT RECEIVER CONTROL",
                                true, "plategrant")) {
                    act.kind = FittedModulesAction::Kind::SetTune;
                    act.file = m.file;
                    act.flag = !m.tuneAllowed;
                }
                py += keyH + 8.0f;
            }

            // WHAT REMOVING KEEPS - read off the remove path, one setting at a
            // time, rather than copied from the design.
            //
            // AppWindow::removeInstalledPlugin unloads, deletes the file and
            // rescans, and touches none of the four. pluginsStopped_,
            // pluginTuneAllowed_ and pluginMuteOverride_ are written back
            // whole by currentConfig() (app_window.cpp:11626-11633) and nothing
            // on the remove path prunes an entry whose file has gone - the
            // diagnostics panel even lists surviving grants under "not
            // currently installed - grant remembered". The map page's
            // rectangle is folded into mapPagesSaved_ BEFORE a page whose
            // plugin vanished is erased (app_window.cpp:7016), and that store
            // is only ever upserted, so the geometry outlives the module.
            //
            // AND WHAT THE FOUR ARE KEYED ON, which is the half the sentence
            // used to leave out. The stop, the grant and the mute all key on
            // core::pluginKey() - the module's FILE NAME - so they are found
            // again by that same file and by nothing else. The map page keys
            // on the module's own display name instead, which is why it is
            // stated separately rather than lumped in with "them".
            static const char* kKept =
                "Removing deletes the file and nothing else. The stop, the "
                "receiver-control grant and the mute setting are all remembered against "
                "this module's FILE NAME, and any map page's position against the name "
                "the module calls itself, so fitting this same file again finds every one "
                "of them as it was. A build that arrives under a different file name is a "
                "different key, and starts from the defaults.";
            drawNote(pdl, ImVec2(po.x, py), innerW, theme::kBrassShade, kKept);
            py += noteHeight(innerW, kKept) + 8.0f;

            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(po.x, py));
            ImGui::Dummy(ImVec2(innerW, 0.0f));
        }
    }
    ImGui::EndChild();

    // With nothing selectable there is no plate to disarm the confirm on, so
    // it is disarmed here instead. A filter change must not leave a delete
    // armed on a module the user can no longer see.
    if (visible.empty()) { deck.confirmRemove.clear(); }

    ImGui::PopID();
    return act;
}

}  // namespace cascade::gui
