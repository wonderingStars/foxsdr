// plugins_view.cpp - the FITTED MODULES window. See plugins_view.hpp for what
// this window is for and why it is not the plugin store.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/plugins_view.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_run.hpp"
#include "core/utf8_text.hpp"
#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {
using cascade::i18n::tr;
using cascade::i18n::trId;

// ============================================================================
// What the window SAYS. No ImGui below this line until the drawing section.
// ============================================================================

namespace {

// The bits that make a module something PluginRunner can feed - it creates an
// instance for a decoder, an I/Q decoder, an image decoder or (host API level 1) an
// in-chain audio processor, and for nothing else. A module with none of them is fed
// nothing, by design and not by fault, which is the whole of the NoSignal state.
constexpr std::uint32_t kSignalCaps = CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER |
                                      CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_AUDIO_PROCESSOR;

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

}  // namespace

FittedState fittedState(const FittedModule& m, bool receiverRunning) {
    // REFUSED FIRST, because a refused record has no descriptor at all: its name,
    // version and capability bits are empty, so every other question below would be
    // asked of fields the host never filled in.
    if (!m.loaded) { return FittedState::Refused; }
    // STOPPED SECOND, and ahead of every "why is nothing happening" answer. The user's
    // own choice is not a fault and must not be reported as one.
    if (m.stopped) { return FittedState::Stopped; }
    // A module that declares no decoder is fed nothing however the receiver is set.
    // Calling that "not fed" would put a warning on a basemap doing exactly what it was
    // fitted to do.
    if ((m.capabilities & kSignalCaps) == 0u) { return FittedState::NoSignal; }
    // THE RECEIVER'S RUN STATE IS PART OF THE ANSWER. isFeeding() says the runner holds
    // an instance matched to the rate the pipeline is CONFIGURED for; with the receiver
    // stopped, that instance is handed nothing. Reading it as FED would put a green lamp
    // on a silent radio.
    if (m.fed && receiverRunning) { return FittedState::Fed; }
    // IDLE sits BETWEEN FED AND NOT DECODING (0.99.73), and the order is the point. A dormant
    // module has no instance, so it can never be fed - but it is not failing to be: nothing is
    // using it, and it starts by itself when something does. Calling that NOT DECODING would put
    // a warning on every module the owner asked to cost nothing while unused.
    if (m.idle) { return FittedState::Idle; }
    return FittedState::NotFed;
}

const char* fittedStateWord(FittedState s) {
    switch (s) {
        case FittedState::Fed: return tr("FED");
        // THE CHIP'S WORD (an existing key): the chip that counts these rows is lettered NOT
        // DECODING, and a row and its chip must not name one state two ways.
        case FittedState::NotFed: return tr("NOT DECODING");
        case FittedState::NoSignal: return tr("TAKES NO SIGNAL");
        // THE CHIP'S WORD again (an existing key). That the user did it is said by the verdict
        // sentence ("You stopped this module. ..."), not by the state word.
        case FittedState::Stopped: return tr("STOPPED");
        case FittedState::Refused: return tr("REFUSED");
        case FittedState::Idle: return tr("IDLE");
    }
    return tr("UNKNOWN");
}

bool fittedOffersStart(const FittedModule& m) { return m.stopped || m.idle; }

const char* fittedKeyWord(const FittedModule& m) {
    return fittedOffersStart(m) ? tr("START") : tr("STOP");
}

std::string fittedStateSentence(const FittedModule& m, bool receiverRunning) {
    switch (fittedState(m, receiverRunning)) {
        case FittedState::Refused:
            // VERBATIM. This string is the answer to "my plugin does not appear" and it
            // carries numbers a paraphrase would throw away ("expected 3, plugin reports
            // 2"). The framing that introduces it is drawn separately, so nothing is added
            // to the host's words.
            return m.error.empty()
                       ? std::string(tr("The host refused this file and recorded no reason."))
                       : m.error;
        case FittedState::Stopped:
            return tr("You stopped this module. It stays installed and its code stays "
                      "mapped, and it is given no decoders, no map targets and no panels of "
                      "its own until you start it again.");
        case FittedState::NoSignal:
            return tr("Fitted, and it takes no signal. This module declares no decoder, so "
                      "nothing is routed to it and nothing should be - it works through the "
                      "capabilities listed above.");
        case FittedState::Fed:
            return tr("Fitted and being fed. The receiver is running and this module has a "
                      "decoder matched to the rate it is producing.");
        case FittedState::Idle:
            return tr("Fitted, and idle. Nothing is using this module, so it is not running and "
                      "costs nothing. It starts by itself when you open its window, its map or "
                      "the output it writes to, and goes quiet again 30 seconds after you finish "
                      "with it. Tick KEEP RUNNING to have it run all the time.");
        case FittedState::NotFed:
            break;
    }
    // NOT FED, which is the state with something to explain. The receiver's own run
    // state is checked FIRST because it stops every module at once, and the runner's
    // per-module sentence would be true and beside the point.
    //
    // IT STILL MATTERS WHETHER THERE IS A SECOND REASON. "Start the receiver and this
    // module is fed" is only true when the runner already holds a matched instance for
    // it; said to a module that ALSO has a rate mismatch it is a promise the product
    // cannot keep.
    if (!receiverRunning) {
        if (m.fed) {
            return tr("Fitted, and fed nothing because the receiver is stopped. It has a "
                      "decoder matched to the rate the receiver is set to, so starting the "
                      "receiver is all this needs.");
        }
        if (!m.idleDetail.empty()) {
            return cascade::core::formatText(
                tr("Fitted, and fed nothing because the receiver is stopped. There is a "
                   "second reason as well: %s"),
                m.idleDetail.c_str());
        }
        return tr("Fitted, and fed nothing because the receiver is stopped.");
    }
    if (!m.idleDetail.empty()) {
        // The runner's own ready-to-display sentence, quoted rather than rewritten: the
        // Plugins rail prints this exact string, and one idle decoder described two ways
        // in two places is worse than one description in the wrong place.
        return m.idleDetail;
    }
    return tr("Fitted, and not being fed. No reason was recorded for it.");
}

std::string fittedOrphanSentence(const FittedModule& m) {
    if (!m.orphaned) { return std::string(); }
    // THE HOST'S WORDS ARE QUOTED, never rewritten: they carry the numbers a paraphrase
    // would lose, exactly as the refusal sentence above does.
    if (m.error.empty()) { return tr("Not installed from the plugin store and not running."); }
    return cascade::core::formatText(tr("Not installed from the plugin store and not running: %s"),
                                     m.error.c_str());
}

FittedRowNote fittedRowNote(const FittedModule& m, bool receiverRunning) {
    FittedRowNote n;
    const FittedState st = fittedState(m, receiverRunning);
    if (m.orphaned) {
        n.text = fittedOrphanSentence(m);
        n.refusal = true;
        return n;
    }
    if (st == FittedState::Refused) {
        n.text = fittedStateSentence(m, receiverRunning);
        n.refusal = true;
        return n;
    }
    // Only a module that reaches outward carries a warning under its name; a module that
    // "publishes to the host only" carries nothing.
    if ((m.capabilities &
         (CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) != 0u) {
        n.text = moduleReachSummary(makeModulePlate(m));
        n.warning = true;
    }
    return n;
}

ModulePlate makeModulePlate(const FittedModule& m) {
    ModulePlate p;

    // WAS A DESCRIPTOR EVER READ OUT OF THIS FILE? The host copies name, version, author
    // and licence only AFTER validatePluginDesc accepts the descriptor, and validation
    // itself requires a non-empty name - so a non-empty name here means the four fields
    // were read, and an empty one means the file was refused before anything was.
    //
    // THE TEST IS THE RECORD, NOT `loaded`. The duplicate resolver turns a module off
    // after loading it, leaving loaded false on a record whose identity is entirely
    // known; keying this off `loaded` would hatch out four facts we hold.
    const bool descriptorRead = !m.name.empty();
    p.haveDescriptor = descriptorRead;

    p.name = m.loaded ? m.name : m.file;
    p.version = m.version;
    p.maker = m.author;
    p.licence = m.licence;
    p.fileName = m.file;

    // FITTED IS TRUE FOR EVERY RECORD HERE, refused ones included: a refused module is a
    // file sitting in the plugins directory, which is exactly what fitted means.
    p.fitted = true;
    p.loaded = m.loaded;
    // The plate's own definition: loaded AND not stopped. Deliberately not this window's
    // finer FED, which also asks whether anything is reaching it.
    p.running = m.loaded && !m.stopped;
    p.refusalReason = m.error;

    // Only a loaded module has had its descriptor read, so only a loaded module's
    // capability word is known. A refused record's bits are 0 because nothing was read,
    // which is "not known" and not "declares nothing".
    p.haveCapabilities = m.loaded;
    p.capabilities = m.capabilities;

    // The grant is a real per-module setting whatever the module asks for, so it is always
    // looked up - but it is only meaningful for a module that can ask.
    p.haveTuneGrant = m.loaded;
    p.tuneGranted = m.tuneAllowed;

    p.haveSizeBytes = (m.sizeBytes > 0u);
    p.sizeBytes = m.sizeBytes;

    // The ABI the install record says the fitted build was made for, when there is one.
    // 0 is "not recorded", which the page reads as exactly that and never as a mismatch.
    p.haveAbi = m.abiVersion != 0u;
    p.abiVersion = m.abiVersion;
    p.hostAbiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    return p;
}

FittedCounts countStates(const std::vector<FittedModule>& modules, bool receiverRunning) {
    FittedCounts c;
    for (const FittedModule& m : modules) {
        ++c.total;
        // NOTHING IS ADDED TOGETHER HERE. NoSignal used to be counted into notFed, and the
        // strip lettered that total "NOT DECODING" with a lit lamp over it - so a basemap,
        // which declares no decoder and can never decode anything, was reported as a
        // decoder that is not decoding.
        switch (fittedState(m, receiverRunning)) {
            case FittedState::Fed: ++c.fed; break;
            case FittedState::NotFed: ++c.notFed; break;
            case FittedState::NoSignal: ++c.noSignal; break;
            case FittedState::Stopped: ++c.stopped; break;
            case FittedState::Refused: ++c.refused; break;
            case FittedState::Idle: ++c.idle; break;
        }
    }
    return c;
}

std::string fittedVerdictLine(bool receiverRunning, const std::string& directory) {
    std::string out =
        receiverRunning
            ? std::string(tr("The receiver is running, so a module with a matched decoder is being fed."))
            : std::string(tr("The receiver is stopped, so NOTHING is being fed to any module however it "
                             "is set. That is why no module below reads FED."));
    out += "  \xc2\xb7  ";
    // WHERE THE SCAN LOOKED, never truncated to nothing: "my plugin does not appear" is
    // sometimes answered by the module having been dropped into the other one of the two
    // directories PluginHost chooses between.
    out += directory.empty() ? std::string(tr("No directory has been scanned yet."))
                             : cascade::core::formatText(tr("read from %s"), directory.c_str());
    return out;
}

std::vector<int> fittedVisibleRows(const std::vector<FittedModule>& modules, bool receiverRunning,
                                   const FittedModulesDeck& deck) {
    const std::string q = lowerAscii(std::string(deck.search));
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(modules.size()); ++i) {
        const FittedModule& m = modules[static_cast<std::size_t>(i)];
        bool show = true;
        switch (fittedState(m, receiverRunning)) {
            case FittedState::Fed: show = deck.showFed; break;
            case FittedState::NotFed: show = deck.showIdle; break;
            case FittedState::NoSignal: show = deck.showNoSignal; break;
            case FittedState::Stopped: show = deck.showStopped; break;
            case FittedState::Refused: show = deck.showRefused; break;
            case FittedState::Idle: show = deck.showIdleDormant; break;
        }
        if (!show) { continue; }
        if (!q.empty()) {
            const std::string hay = lowerAscii(m.name + " " + m.file + " " + m.version);
            if (hay.find(q) == std::string::npos) { continue; }
        }
        rows.push_back(i);
    }
    const auto label = [&](int i) {
        const FittedModule& m = modules[static_cast<std::size_t>(i)];
        return lowerAscii(m.loaded && !m.name.empty() ? m.name : m.file);
    };
    std::sort(rows.begin(), rows.end(), [&](int a, int b) {
        const std::string la = label(a), lb = label(b);
        if (la != lb) { return la < lb; }
        return a < b;
    });
    return rows;
}

std::string fittedDateText(const FittedModule& m) {
    if (m.fittedAtUnix <= 0) { return std::string(); }
    const std::time_t t = static_cast<std::time_t>(m.fittedAtUnix);
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) != 0) { return std::string(); }
#else
    if (localtime_r(&t, &tmv) == nullptr) { return std::string(); }
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday);
    return cascade::core::formatText(tr("fitted %s"), buf);
}

FittedModule makeFittedModule(const cascade::core::LoadedPlugin& p, bool stopped, bool fed,
                              std::string idleDetail, bool tuneAllowed, bool idle,
                              bool keepRunning) {
    FittedModule m;
    m.file = cascade::core::pluginKey(p);
    m.id = m.file;
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
    m.idle = idle;
    m.keepRunning = keepRunning;
    m.idleDetail = std::move(idleDetail);
    // From the TABLE POINTER and not from the capability bit. The host clears a table it
    // could not accept, so a module that declared the bit and supplied nothing usable would
    // otherwise be offered a grant it cannot use.
    m.tuneCapable = (p.hostClient != nullptr);
    m.tuneAllowed = tuneAllowed;
    // THE RECORD'S SIZE, measured once by the scan (LoadedPlugin::fileBytes) and carried,
    // never looked up here: this runs for every module on every frame the window is open,
    // and a stat per module per frame froze windows whose plugin folder was slow. 0 is "not
    // measured" and the page draws no size for it.
    m.sizeBytes = p.fileBytes;
    return m;
}

// ============================================================================
// The window.
// ============================================================================

namespace {

float S() { return uiscale::factor(); }

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}
float faceH(ImFont* f, float px) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, "Ag").y; }
float wrapH(ImFont* f, float px, float w, const char* s) {
    if (s == nullptr || s[0] == '\0') { return 0.0f; }
    if (w < 16.0f) { return faceH(f, px); }
    return f->CalcTextSizeA(px, FLT_MAX, w, s).y;
}

void censusRect(const std::string& name, float x0, float y0, float x1, float y1) {
    if (census::enabled()) { census::rect(name, x0, y0, x1, y1); }
}

// The lamp colour for a state.
//
// AMBER IS NOT USED HERE. The palette's rule is that amber is a READING - the counts on
// the chips are amber because they are measurements. A state that wants looking at takes
// kGold, so a lamp and a number can never be mistaken for one another.
ImU32 stateLamp(FittedState s) {
    switch (s) {
        case FittedState::Fed: return theme::kPhosphor;
        case FittedState::NotFed: return theme::kGold;
        case FittedState::NoSignal: return theme::kBrassTint;
        case FittedState::Stopped: return theme::kBrassTint;
        case FittedState::Refused: return theme::kAlarm;
        // A dormant module is working as designed: the same quiet brass as a stopped one, and
        // never gold, which is reserved for something that wants looking at.
        case FittedState::Idle: return theme::kBrassTint;
    }
    return theme::kBrassTint;
}

// The ink the state WORD is lettered in. Deliberately not the lamp colour for every state:
// a stopped module is a choice the user made, so it letters in plain cream rather than in
// anything that reads as trouble.
ImU32 stateInk(FittedState s) {
    switch (s) {
        case FittedState::Fed: return theme::kPhosphor;
        case FittedState::NotFed: return theme::kGold;
        case FittedState::NoSignal: return theme::kInkMuted;
        case FittedState::Stopped: return theme::kCream;
        case FittedState::Refused: return theme::kAlarmHot;
        case FittedState::Idle: return theme::kInkMuted;
    }
    return theme::kInkMuted;
}

// Whether a lamp is LIT. Only a module actually being fed lights one: a panel of lit lamps
// means nothing, and the whole point of this window is that a green lamp is worth
// something.
bool stateLampLit(FittedState s) { return s == FittedState::Fed || s == FittedState::Refused; }

// The census word of a state: what the tests read a row's state and a chip's rectangle by.
const char* censusStateName(FittedState s) {
    switch (s) {
        case FittedState::Fed: return "fed";
        case FittedState::NotFed: return "notfed";
        case FittedState::NoSignal: return "nosignal";
        case FittedState::Stopped: return "stopped";
        case FittedState::Refused: return "refused";
        case FittedState::Idle: return "idle";
    }
    return "unknown";
}

// The kind's glyph for a module the catalogue does not know: from what it declares, and the
// dial for anything else (a module nobody could read has no kind to draw).
std::string kindGlyph(const FittedModule& m) {
    if (m.category.empty() && m.loaded) {
        if ((m.capabilities & (CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER)) != 0u) {
            return "broadcast";
        }
        if ((m.capabilities & (CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO)) != 0u) {
            return "maps-tools";
        }
    }
    return m.category;
}

// A state chip: a lamp, the count in the figure face, and the word. A toggle - on is
// outlined in the lamp's colour, off is drawn faint.
float chipWidth(const char* word, int count) {
    char n[16];
    std::snprintf(n, sizeof n, "%d", count);
    const float px = fonts::tinyPx();
    return 30.0f * S() + textW(fonts::ui(), px * 1.25f, n) + 8.0f * S() +
           trackedWidth(fonts::legend(), px, word, px * 0.08f) + 12.0f * S();
}

bool drawChip(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* word, int count,
              ImU32 lamp, bool lit, bool on, const char* id) {
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(tl);
    const bool pressed = ImGui::InvisibleButton("##chip", ImVec2(br.x - tl.x, br.y - tl.y));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const float k = S();
    const ImU32 line = on ? theme::withAlpha(lamp, 0.85f) : theme::withAlpha(theme::kBrassDark, 0.9f);
    if (on) { dl->AddRectFilled(tl, br, theme::withAlpha(lamp, hovered ? 0.16f : 0.08f), 2.0f); }
    else if (hovered) { dl->AddRectFilled(tl, br, theme::withAlpha(theme::kBrassMid, 0.2f), 2.0f); }
    dl->AddRect(tl, br, line, 2.0f, 0, 1.0f);
    const float cy = (tl.y + br.y) * 0.5f;
    drawBenchLamp(dl, ImVec2(tl.x + 14.0f * k, cy), 4.5f * k, lamp, lit && on, nullptr);
    const float px = fonts::tinyPx();
    char n[16];
    std::snprintf(n, sizeof n, "%d", count);
    ImFont* rf = fonts::ui();
    const float rpx = px * 1.25f;
    float x = tl.x + 26.0f * k;
    dl->AddText(rf, rpx, ImVec2(x, cy - faceH(rf, rpx) * 0.5f),
                on ? (count > 0 ? theme::kAmber : theme::kAmberDim) : theme::kInkFaint, n);
    x += textW(rf, rpx, n) + 8.0f * k;
    ImFont* lf = fonts::legend();
    addTrackedText(dl, lf, px, ImVec2(x, cy - faceH(lf, px) * 0.5f), on ? theme::kCream : theme::kInkFaint,
                   word, px * 0.08f, br.x - 4.0f);
    return pressed;
}

constexpr float kReachRowGap = 8.0f;

const char* kReachRefusedText = FOX_TR_NOOP(
    "Not known here, and nothing is routed to it. This file is fitted and the host did "
    "not accept it, so no capability list reached this panel and none of the module is "
    "loaded. That is not the same as a module which declares nothing.");
const char* kReachTuneNote = FOX_TR_NOOP(
    "The tune grant above and the radio-settings grant are the only permissions this "
    "console does enforce: without them every request to retune, or to change the "
    "receiver's settings, is refused. Nothing else in the list is a gate.");
const char* kReachNoTuneNote = FOX_TR_NOOP(
    "The only permissions this console enforces are the per-module tune and "
    "radio-settings grants, and this module asks for neither. Nothing else in the list "
    "is a gate.");
const char* kSettingsNote = FOX_TR_NOOP(
    "Radio settings lets this module change the mode, bandwidth, squelch, gains, sample "
    "rate, volume and mute, and start or stop the receiver. It is separate from receiver "
    "control, which only lets it tune.");
const char* kReachLeadText = FOX_TR_NOOP(
    "Declared by the maker, not enforced. A fitted module is loaded into this "
    "application's own process and runs with every privilege the application has: "
    "there is no sandbox and no permission model. This list is what the module says "
    "it PROVIDES, not a limit on what it can take.");
const char* kOrphanText = FOX_TR_NOOP(
    "This file was not installed from the plugin store - it has no install record and the "
    "catalogue does not list it - and it is not running, so FoxSDR has left it where it is. "
    "REMOVE FILE deletes this one file and nothing else.");
const char* kKeptText = FOX_TR_NOOP(
    "Removing deletes the file and nothing else. The stop, the "
    "receiver-control and radio-settings grants and the mute setting are all "
    "remembered against this module's FILE NAME, and any map page's position "
    "and the module's own settings against the name the module calls itself, "
    "so fitting this same file again finds every one of them as it was. A "
    "build that arrives under a different file name is a different key, and "
    "starts from the defaults, apart from its own settings.");

// The two-step REMOVE key's words and widths, measured across all three so the key does not
// change width when it is pressed and drag the row with it.
float removeKeyWidth() {
    return std::max({outlineKeyWidth(tr("REMOVE")), outlineKeyWidth(tr("REMOVE FILE")),
                     outlineKeyWidth(tr("CONFIRM"))});
}
float stopKeyWidth() {
    return std::max(chassisKeyWidth(tr("STOP")), chassisKeyWidth(tr("START")));
}
float stateColumnWidth() {
    ImFont* lf = fonts::legend();
    const float px = fonts::tinyPx();
    float w = 0.0f;
    for (FittedState s : {FittedState::Fed, FittedState::NotFed, FittedState::NoSignal,
                          FittedState::Stopped, FittedState::Refused, FittedState::Idle}) {
        w = std::max(w, trackedWidth(lf, px, fittedStateWord(s), px * 0.10f));
    }
    return w + 22.0f * S();
}

// Draws one reach row ("o Audio decoder / the sentence under it") and returns its height.
float drawReachRow(ImDrawList* dl, const ReachRow& r, float x, float y, float w) {
    ImFont* uf = fonts::ui();
    const float px = fonts::uiPx();
    const float k = S();
    const ImVec2 c(x + 6.0f * k, y + faceH(uf, px) * 0.5f);
    if (r.outward) {
        dl->AddCircleFilled(c, 5.0f * k, theme::withAlpha(theme::kGold, 0.28f), 12);
        dl->AddCircleFilled(c, 3.2f * k, theme::kGold, 12);
    } else {
        dl->AddCircle(c, 3.6f * k, theme::kInkFaint, 12, 1.5f);
    }
    const float tx = x + 20.0f * k;
    dl->AddText(uf, px, ImVec2(tx, y), r.outward ? theme::kIvory : theme::kCream, r.key.c_str());
    const float h1 = faceH(uf, px) + 2.0f * k;
    dl->AddText(uf, px, ImVec2(tx, y + h1), theme::kInkMuted, r.detail.c_str(), nullptr, w - 20.0f * k);
    return h1 + wrapH(uf, px, w - 20.0f * k, r.detail.c_str()) + kReachRowGap * k;
}

// THE ON THIS MACHINE BOX of a module's page. In the order the coordinator fixed: the state
// word with its lamp, the reach list (with the grants and the module's own keys), the verdict
// sentence - or, for a module the host refused, WHY IT IS NOT RUNNING - the notes, then FILE,
// PLUGIN ABI and when it was fitted. Returns the y below the box.
float drawOnThisMachine(ImDrawList* dl, const FittedModule& m, const ModulePlate& plate,
                        bool receiverRunning, float x, float y, float W,
                        FittedModulesAction& act, FittedModulesDeck& deck) {
    (void)deck;
    ImFont* uf = fonts::ui();
    ImFont* lf = fonts::legend();
    const float px = fonts::uiPx();
    const float k = S();
    const float pad = 16.0f * k;
    const float inner = W - pad * 2.0f;
    const FittedState st = fittedState(m, receiverRunning);
    const float y0 = y;
    y += drawSectionHeading(dl, ImVec2(x, y), W, tr("ON THIS MACHINE"));
    const float boxTop = y;

    // The box is drawn after its contents are measured; the contents are laid out into a
    // list of draw steps measured first, so the border is the right height. Simplest honest
    // way: lay out once with a null-effect pass for height, then draw. The pieces below are
    // all cheap, so the layout runs twice.
    const auto layout = [&](bool draw, float& outH) {
        float cy = boxTop + pad;
        // 1. the state word with its lamp
        {
            const float wpx = px * 1.15f;
            const char* word = fittedStateWord(st);
            if (draw) {
                drawBenchLamp(dl, ImVec2(x + pad + 7.0f * k, cy + faceH(lf, wpx) * 0.5f), 6.0f * k,
                              stateLamp(st), stateLampLit(st), nullptr);
                addTrackedText(dl, lf, wpx, ImVec2(x + pad + 24.0f * k, cy), stateInk(st), word,
                               wpx * 0.10f);
            }
            cy += faceH(lf, wpx) + 12.0f * k;
        }
        // 2. what this module reaches: the reach list, the grants, the module's own keys
        {
            if (draw) {
                addTrackedText(dl, lf, std::max(11.0f, fonts::tinyPx() * 0.9f), ImVec2(x + pad, cy),
                               theme::kInkMuted, tr("WHAT THIS MODULE REACHES"),
                               std::max(11.0f, fonts::tinyPx() * 0.9f) * 0.14f, x + W - pad);
            }
            cy += faceH(lf, std::max(11.0f, fonts::tinyPx() * 0.9f)) + 8.0f * k;
            const std::vector<ReachRow> rows = moduleReachRows(plate);
            if (rows.empty()) {
                const char* why = tr(kReachRefusedText);
                if (draw) { dl->AddText(uf, px, ImVec2(x + pad, cy), theme::kGold, why, nullptr, inner); }
                cy += wrapH(uf, px, inner, why) + 8.0f * k;
            } else {
                const char* lead = tr(kReachLeadText);
                if (draw) { dl->AddText(uf, px, ImVec2(x + pad, cy), theme::kInkMuted, lead, nullptr, inner); }
                cy += wrapH(uf, px, inner, lead) + 10.0f * k;
                for (const ReachRow& r : rows) {
                    if (draw) {
                        cy += drawReachRow(dl, r, x + pad, cy, inner);
                    } else {
                        cy += faceH(uf, px) + 2.0f * k + wrapH(uf, px, inner - 20.0f * k, r.detail.c_str()) +
                              kReachRowGap * k;
                    }
                }
                const char* note = tr((plate.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u ? kReachTuneNote
                                                                                           : kReachNoTuneNote);
                if (draw) { dl->AddText(uf, px, ImVec2(x + pad, cy), theme::kInkMuted, note, nullptr, inner); }
                cy += wrapH(uf, px, inner, note) + 10.0f * k;
            }
            // THE ONE REACH THAT IS ENFORCED, and therefore the one that gets a key: per module,
            // off by default, and without it PluginUi answers every request_tune DENIED. Offered
            // only to a module that ASKS - a control that changes a setting nothing reads is a
            // control that lies about having done something.
            const float kh = outlineKeyHeight();
            const auto grantKey = [&](const char* label, bool on, const char* id, FittedModulesAction::Kind kind,
                                      bool flag) {
                const float kw = outlineKeyWidth(label) + 20.0f * k;
                if (draw) {
                    if (drawOutlineKey(dl, ImVec2(x + pad, cy), ImVec2(x + pad + kw, cy + kh), label,
                                       on ? theme::kInkMuted : theme::kPhosphor, true, id, nullptr)) {
                        act.kind = kind;
                        act.file = m.file;
                        act.flag = flag;
                    }
                }
                cy += kh + 8.0f * k;
            };
            if (m.loaded && m.tuneCapable) {
                grantKey(m.tuneAllowed ? tr("REVOKE RECEIVER CONTROL") : tr("GRANT RECEIVER CONTROL"),
                         m.tuneAllowed, "granttune", FittedModulesAction::Kind::SetTune, !m.tuneAllowed);
            }
            if (m.loaded && m.settingsCapable) {
                grantKey(m.settingsAllowed ? tr("REVOKE RADIO SETTINGS") : tr("GRANT RADIO SETTINGS"),
                         m.settingsAllowed, "grantsettings", FittedModulesAction::Kind::SetSettings,
                         !m.settingsAllowed);
                const char* sn = tr(kSettingsNote);
                if (draw) { dl->AddText(uf, px, ImVec2(x + pad, cy), theme::kInkMuted, sn, nullptr, inner); }
                cy += wrapH(uf, px, inner, sn) + 8.0f * k;
            }
            // The module's own command keys, in its own words, two to a row.
            if (m.loaded && !m.stopped && !m.commands.empty()) {
                const float halfW = (inner - 8.0f * k) * 0.5f;
                for (std::size_t ci = 0; ci < m.commands.size(); ++ci) {
                    const bool left = (ci % 2u) == 0u;
                    const float kx = left ? x + pad : x + pad + halfW + 8.0f * k;
                    if (draw) {
                        char kid[32];
                        std::snprintf(kid, sizeof kid, "cmd%zu", ci);
                        if (drawOutlineKey(dl, ImVec2(kx, cy), ImVec2(kx + halfW, cy + kh),
                                           m.commands[ci].label.c_str(), theme::kInkMuted, true, kid, nullptr)) {
                            act.kind = FittedModulesAction::Kind::Command;
                            act.file = m.file;
                            act.commandId = m.commands[ci].id;
                        }
                    }
                    if (!left || ci + 1u == m.commands.size()) { cy += kh + 8.0f * k; }
                }
            }
        }
        // 3. the verdict sentence - or why it is not running
        {
            const std::string sentence = fittedStateSentence(m, receiverRunning);
            const bool refused = st == FittedState::Refused;
            float h = 0.0f;
            if (refused) {
                const float cpx = std::max(11.0f, fonts::tinyPx() * 0.9f);
                if (draw) {
                    addTrackedText(dl, lf, cpx, ImVec2(x + pad, cy), theme::kInkMuted,
                                   tr("WHY IT IS NOT RUNNING"), cpx * 0.14f, x + W - pad);
                }
                cy += faceH(lf, cpx) + 6.0f * k;
            }
            h = wrapH(uf, px, inner, sentence.c_str());
            if (draw) {
                // Plain ink for the verdict: the state word above it already carries the tone, and a
                // paragraph lettered in gold or green reads as an alert.
                dl->AddText(uf, px, ImVec2(x + pad, cy), refused ? theme::kAlarmHot : theme::kCream,
                            sentence.c_str(), nullptr, inner);
            }
            cy += h + 10.0f * k;
        }
        // 3b. KEEP RUNNING (0.99.73): the tick that pins a module to ALWAYS. Offered to a module that
        // can be idle at all - not to a refused one, a stopped one (START is its key), a module with
        // nothing to start, or one that is always in use (a processor in the audio chain).
        if (m.loaded && !m.stopped && cascade::core::pluginCapsHaveLifecycle(m.capabilities) &&
            !cascade::core::pluginCapsStandingDuty(m.capabilities)) {
            const ImGuiStyle& gs = ImGui::GetStyle();
            const float tickH = px + gs.FramePadding.y * 2.0f;
            if (draw) {
                bool ticked = m.keepRunning;
                ImGui::SetCursorScreenPos(ImVec2(x + pad, cy));
                ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kIvory));
                ImGui::PushStyleColor(ImGuiCol_CheckMark, theme::vec(theme::kPhosphor));
                ImGui::PushFont(uf, px / uiscale::factor());
                if (ImGui::Checkbox(trId("Keep running"), &ticked)) {
                    act.kind = FittedModulesAction::Kind::SetKeepRunning;
                    act.file = m.file;
                    act.flag = ticked;
                }
                censusRect("fitted:page:keeprunning", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                           ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tr("Ticked, this module runs all the time, as every module used to. "
                                               "Unticked, it runs only while something is using it - its window or "
                                               "map, the radar scope, a patch, the Decoder output window - and goes "
                                               "quiet 30 seconds after."));
                }
                ImGui::PopFont();
                ImGui::PopStyleColor(2);
            }
            cy += tickH + 8.0f * k;
        }
        // 4. the notes: an orphan, a changed file, the module's own warning
        const auto note = [&](const std::string& text, ImU32 col) {
            if (text.empty()) { return; }
            if (draw) { dl->AddText(uf, px, ImVec2(x + pad, cy), col, text.c_str(), nullptr, inner); }
            cy += wrapH(uf, px, inner, text.c_str()) + 8.0f * k;
        };
        if (m.orphaned) { note(tr(kOrphanText), theme::kGold); }
        note(m.integrityNote, theme::kAlarmHot);
        if (m.loaded && !m.notice.empty()) {
            note(m.notice, m.noticeLevel >= CASCADE_LOG_ERROR ? theme::kAlarmHot : theme::kGold);
        }
        // 5. FILE, PLUGIN ABI, fitted <date>
        {
            const float lw = 150.0f * k;
            const float cpx = std::max(11.0f, fonts::tinyPx() * 0.9f);
            const auto kv = [&](const char* key, const std::string& value, ImU32 col) {
                if (value.empty()) { return; }
                const float vh = wrapH(uf, px, inner - lw, value.c_str());
                if (draw) {
                    addTrackedText(dl, lf, cpx, ImVec2(x + pad, cy + 2.0f * k), theme::kInkMuted, key,
                                   cpx * 0.14f, x + pad + lw);
                    dl->AddText(uf, px, ImVec2(x + pad + lw, cy), col, value.c_str(), nullptr, inner - lw);
                }
                cy += std::max(vh, faceH(uf, px)) + 6.0f * k;
            };
            kv(tr("FILE"), m.path.empty() ? std::string(tr("not recorded")) : m.path,
               m.path.empty() ? theme::kInkFaint : theme::kInkMuted);
            std::string abi;
            if (m.abiVersion == 0u) {
                abi = tr("not recorded");
            } else if (m.abiVersion == static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION)) {
                abi = cascade::core::formatText(tr("%u, matches this build"), m.abiVersion);
            } else {
                abi = cascade::core::formatText(tr("%u, this build needs %u"), m.abiVersion,
                                                static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
            }
            kv(tr("PLUGIN ABI"), abi, m.abiVersion == 0u ? theme::kInkFaint : theme::kIvory);
            const std::string fitted = fittedDateText(m);
            if (!fitted.empty()) {
                if (draw) { dl->AddText(uf, px, ImVec2(x + pad, cy), theme::kInkMuted, fitted.c_str()); }
                cy += faceH(uf, px) + 6.0f * k;
            }
        }
        outH = cy - boxTop + pad - 6.0f * k;
    };
    float boxH = 0.0f;
    layout(false, boxH);
    dl->AddRectFilled(ImVec2(x, boxTop), ImVec2(x + W, boxTop + boxH), theme::kEnamelDark);
    dl->AddRect(ImVec2(x, boxTop), ImVec2(x + W, boxTop + boxH), theme::withAlpha(theme::kBrassDark, 0.9f),
                0.0f, 0, 1.0f);
    float drawnH = 0.0f;
    layout(true, drawnH);
    if (census::enabled()) {
        census::note("fitted:section:", "onthismachine");
        census::rect("fitted:section:onthismachine", x, y0, x + W, boxTop + boxH);
    }
    return boxTop + boxH;
}

}  // namespace

FittedModulesAction drawFittedModulesPanel(FittedModulesDeck& deck, const FittedModulesModel& model) {
    FittedModulesAction act;

    ImGui::PushID("fittedmodules");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 240.0f || avail.y < 160.0f) {
        // Too small to letter honestly. Say why rather than drawing a clipped panel that
        // looks broken.
        ImGui::TextDisabled("%s", tr("Too narrow to draw. Widen the window."));
        ImGui::PopID();
        return act;
    }

    ImFont* uf = fonts::ui();
    const float upx = fonts::uiPx();
    const float k = S();
    const float pad = 16.0f * k;
    const double now = ImGui::GetTime();
    if (!deck.confirmRemove.empty() && now > deck.confirmUntil) { deck.confirmRemove.clear(); }

    // THE PAGE BOOKKEEPING: an open page is a module FILE; a module that has gone (removed,
    // renamed by a rescan) has no page to show.
    int pageIdx = -1;
    if (!deck.pageFile.empty()) {
        for (int i = 0; i < static_cast<int>(model.modules.size()); ++i) {
            if (model.modules[static_cast<std::size_t>(i)].file == deck.pageFile) {
                pageIdx = i;
                break;
            }
        }
        if (pageIdx < 0) { deck.pageFile.clear(); }
    }
    if (deck.pageFile != deck.lastPageFile) {
        deck.page.reset();
        deck.lastPageFile = deck.pageFile;
        deck.pageOpenedCatalogueId.clear();
        if (pageIdx >= 0) {
            deck.pageOpenedCatalogueId = model.modules[static_cast<std::size_t>(pageIdx)].catalogueId;
        }
    }

    const FittedCounts counts = countStates(model.modules, model.receiverRunning);

    // ======================= THE TOP BAR ============================================
    //
    // ONE ROW: the search field at the left, the five feed-state chips, and at the right the
    // two keys that act on everything at once. A window too narrow for one row puts the chips
    // on a second. Nothing else is above the list but the one verdict line.
    const float keyH = outlineKeyHeight();
    const float chipH = std::max(28.0f * k, keyH);
    struct Chip {
        const char* word;
        int n;
        ImU32 lamp;
        bool lit;
        bool* flag;
        const char* census;  // the state's census word (censusStateName)
    };
    // IDLE sits right after FED, as it does in the order a module is asked in (0.99.73): the
    // modules that cost nothing, beside the ones that are working.
    const Chip chips[6] = {
        {tr("FED"), counts.fed, theme::kPhosphor, counts.fed > 0, &deck.showFed, "fed"},
        {tr("IDLE"), counts.idle, theme::kBrassTint, false, &deck.showIdleDormant, "idle"},
        {tr("NOT DECODING"), counts.notFed, theme::kGold, counts.notFed > 0, &deck.showIdle,
         "notfed"},
        {tr("TAKES NO SIGNAL"), counts.noSignal, theme::kBrassTint, false, &deck.showNoSignal,
         "nosignal"},
        {tr("STOPPED"), counts.stopped, theme::kBrassTint, false, &deck.showStopped, "stopped"},
        {tr("REFUSED"), counts.refused, theme::kAlarm, counts.refused > 0, &deck.showRefused,
         "refused"},
    };
    float chipsW = 0.0f;
    for (const Chip& c : chips) { chipsW += chipWidth(c.word, c.n) + 8.0f * k; }
    const char* scanL = tr("SCAN AGAIN");
    const char* resetL = tr("RESET WINDOW SIZES");
    const float scanW = chassisKeyWidth(scanL), resetW = chassisKeyWidth(resetL);
    const float searchW = std::clamp(avail.x * 0.2f, 180.0f * k, 280.0f * k);
    const float oneRowW = pad * 2.0f + searchW + 18.0f * k + chipsW + 18.0f * k + resetW + 10.0f * k + scanW;
    const bool twoRows = oneRowW > avail.x;
    const float barH = (twoRows ? 56.0f * k + chipH + 14.0f * k : 56.0f * k);
    dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + barH), theme::kEnamelDark);
    dl->AddLine(ImVec2(origin.x, origin.y + barH - 1.0f), ImVec2(origin.x + avail.x, origin.y + barH - 1.0f),
                theme::kBrassDark, 1.0f);
    const float cy1 = origin.y + 28.0f * k;
    {
        KeyRect sr;
        const float fieldH = std::max(30.0f * k, upx + 14.0f * k);
        drawSearchField(ImVec2(origin.x + pad, cy1 - fieldH * 0.5f), searchW, tr("Search fitted modules"),
                        deck.search, sizeof deck.search, &sr);
        censusRect("fitted:search", sr.tl.x, sr.tl.y, sr.br.x, sr.br.y);

        // the two keys, at the right
        // SCAN AGAIN, then RESET WINDOW SIZES at the far right, as the mock-up has them.
        float kx = origin.x + avail.x - pad - resetW;
        KeyRect kr;
        if (drawChassisKey(dl, ImVec2(kx, cy1 - keyH * 0.5f), ImVec2(kx + resetW, cy1 + keyH * 0.5f), resetL,
                           true, "resetwindows", &kr)) {
            act.kind = FittedModulesAction::Kind::ResetWindows;
        }
        // The tooltip names the four kinds of window it moves: "reset" on its own could mean the modules.
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Puts every decoder, panel, instrument and map window back to its "
                                       "default size and position."));
        }
        censusRect("fitted:reset", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);
        kx -= 10.0f * k + scanW;
        if (drawChassisKey(dl, ImVec2(kx, cy1 - keyH * 0.5f), ImVec2(kx + scanW, cy1 + keyH * 0.5f), scanL,
                           true, "scan", &kr)) {
            act.kind = FittedModulesAction::Kind::Rescan;
        }
        censusRect("fitted:scan", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);

        // the chips
        float cx = twoRows ? origin.x + pad : origin.x + pad + searchW + 18.0f * k;
        const float cyc = twoRows ? origin.y + 56.0f * k + chipH * 0.5f - 4.0f * k : cy1;
        for (int i = 0; i < 6; ++i) {
            const float w = chipWidth(chips[i].word, chips[i].n);
            char id[24];
            std::snprintf(id, sizeof id, "chip%d", i);
            if (drawChip(dl, ImVec2(cx, cyc - chipH * 0.5f), ImVec2(cx + w, cyc + chipH * 0.5f), chips[i].word,
                         chips[i].n, chips[i].lamp, chips[i].lit, *chips[i].flag, id)) {
                *chips[i].flag = !*chips[i].flag;
            }
            if (census::enabled()) {
                census::rect(std::string("fitted:chip:") + chips[i].census, cx, cyc - chipH * 0.5f, cx + w,
                             cyc + chipH * 0.5f);
                census::note("fitted:count:", std::string(chips[i].census) + ":" + std::to_string(chips[i].n));
            }
            cx += w + 8.0f * k;
        }
    }
    float y = origin.y + barH + 10.0f * k;
    const float W = avail.x - pad * 2.0f;
    const float x0 = origin.x + pad;

    // THE ONE VERDICT LINE: the receiver's state, and after a dot where the scan looked.
    {
        const std::string line = fittedVerdictLine(model.receiverRunning, model.directory);
        dl->AddText(uf, upx, ImVec2(x0, y), theme::kInkMuted, line.c_str(), nullptr, W);
        y += wrapH(uf, upx, W, line.c_str()) + 8.0f * k;
    }
    // Whatever the last install or remove said, verbatim and in its own colour, because a
    // REMOVE that FAILED is exactly when the user needs telling and the row it acted on has
    // already gone. A refusal the store made itself arrives in English and is translated for
    // the page; PluginRepo's own sentences come back as written (trStoredReason).
    if (!model.error.empty() || !model.report.empty()) {
        const std::string text = !model.error.empty() ? trStoredReason(model.error) : model.report;
        dl->AddText(uf, upx, ImVec2(x0, y), !model.error.empty() ? theme::kAlarmHot : theme::kPhosphor,
                    text.c_str(), nullptr, W);
        y += wrapH(uf, upx, W, text.c_str()) + 8.0f * k;
    }

    // ======================= THE BODY ===============================================
    const float bodyH = std::max(60.0f, origin.y + avail.y - y);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    const bool onPage = pageIdx >= 0;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(theme::kWell));
    ImGui::BeginChild(onPage ? "##fittedpage" : "##fittedlist", ImVec2(avail.x, bodyH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const ImVec2 co = ImGui::GetCursorScreenPos();
        const float cw = std::max(300.0f, ImGui::GetContentRegionAvail().x - pad * 2.0f);
        const float cx0 = co.x + pad;
        float cy = co.y + 12.0f * k;

        if (onPage) {
            // ================= THE MODULE'S PAGE ====================================
            const FittedModule& m = model.modules[static_cast<std::size_t>(pageIdx)];
            const char* bl = tr("< FITTED");
            {
                const float bw = chassisKeyWidth(bl), bh = outlineKeyHeight();
                KeyRect backRect;
                if (drawChassisKey(cdl, ImVec2(cx0, cy), ImVec2(cx0 + bw, cy + bh), bl, true, "back", &backRect)) {
                    deck.pageFile.clear();
                    if (census::enabled()) { census::note("fitted:page:closed:", "back"); }
                }
                censusRect("fitted:page:back", backRect.tl.x, backRect.tl.y, backRect.br.x, backRect.br.y);
                cy += bh + 14.0f * k;
            }
            if (census::enabled()) { census::note("fitted:page:", m.id); }
            // The same column as the store's page (it is the same page body): the window's width
            // less the margins, up to 1180 px.
            const float colW = std::min(cw, 1180.0f * k);
            const FittedState st = fittedState(m, model.receiverRunning);
            // The catalogue's record of this module, when the catalogue knows it.
            const StoreModule* cat = (m.catalogueIndex >= 0 &&
                                      m.catalogueIndex < static_cast<int>(model.catalogue.size()))
                                         ? &model.catalogue[static_cast<std::size_t>(m.catalogueIndex)]
                                         : nullptr;
            // What the module's facts say: the catalogue's plate when there is one (so the
            // two windows' pages agree), with the machine's own state laid over it.
            ModulePlate plate = cat != nullptr ? cat->plate : makeModulePlate(m);
            if (cat != nullptr) {
                const ModulePlate own = makeModulePlate(m);
                plate.fitted = true;
                plate.loaded = own.loaded;
                plate.running = own.running;
                plate.refusalReason = own.refusalReason;
                plate.fileName = own.fileName;
                plate.haveCapabilities = own.haveCapabilities;
                plate.capabilities = own.capabilities;
                plate.haveTuneGrant = own.haveTuneGrant;
                plate.tuneGranted = own.tuneGranted;
            }
            ModulePlate machine = makeModulePlate(m);  // the plate the ON THIS MACHINE box reaches by

            // ---- the header ---------------------------------------------------------
            const float gb = 96.0f * k, hpad = 16.0f * k;
            const std::string title = m.loaded && !m.name.empty() ? m.name : m.file;
            const std::string meta = cascade::core::formatText(
                tr("%s  \xc2\xb7  %s"),
                m.author.empty() ? tr("maker not stated") : m.author.c_str(),
                cascade::core::formatText(tr("version %s"), m.version.empty() ? "?" : m.version.c_str()).c_str());
            const float namePx = fonts::panelPx() * 1.45f;
            const float stopW = stopKeyWidth() + 20.0f * k;
            const float tcw = std::max(80.0f, colW - hpad * 3.0f - gb - stopW);
            const float nameH = wrapH(uf, namePx, tcw, title.c_str());
            const float metaH = wrapH(uf, upx, tcw, meta.c_str());
            const float headH = std::max(gb + hpad * 2.0f, hpad * 2.0f + nameH + 4.0f * k + metaH);
            const ImVec2 htl(cx0, cy), hbr(cx0 + colW, cy + headH);
            cdl->AddRectFilled(htl, hbr, theme::kEnamelDark);
            cdl->AddRect(htl, hbr, theme::withAlpha(theme::kBrassDark, 0.9f), 0.0f, 0, 1.0f);
            const ImVec2 gtl(htl.x + hpad, htl.y + (headH - gb) * 0.5f);
            cdl->AddRectFilled(gtl, ImVec2(gtl.x + gb, gtl.y + gb), theme::kVoid);
            cdl->AddRect(gtl, ImVec2(gtl.x + gb, gtl.y + gb), theme::kBrassDark, 0.0f, 0, 1.0f);
            drawCategoryGlyph(cdl, ImVec2(gtl.x + 2.0f, gtl.y + 2.0f), gb - 4.0f, kindGlyph(m),
                              theme::kPhosphor, 1.6f * (gb - 4.0f) / 56.0f);
            const float tx = gtl.x + gb + hpad;
            cdl->AddText(uf, namePx, ImVec2(tx, htl.y + hpad), theme::kIvory, title.c_str(), nullptr, tcw);
            cdl->AddText(uf, upx, ImVec2(tx, htl.y + hpad + nameH + 4.0f * k), theme::kInkMuted, meta.c_str(),
                         nullptr, tcw);
            if (m.loaded) {
                const float kh = outlineKeyHeight() + 6.0f * k;
                const ImVec2 ktl(hbr.x - hpad - stopW, htl.y + (headH - kh) * 0.5f);
                KeyRect kr;
                // The chassis-grey key of the mock-up: proud metal, the action engraved on it.
                // START for a module that is not running - stopped, or idle (0.99.73) - and STOP for one
                // that is. The key's label is its action, never the state.
                const bool offersStart = fittedOffersStart(m);
                if (drawChassisKey(cdl, ktl, ImVec2(ktl.x + stopW, ktl.y + kh), fittedKeyWord(m),
                                   true, "pagestop", &kr)) {
                    act.kind = offersStart ? FittedModulesAction::Kind::Start : FittedModulesAction::Kind::Stop;
                    act.file = m.file;
                }
                censusRect("fitted:page:key", kr.tl.x, kr.tl.y, kr.br.x, kr.br.y);
            }
            cy = hbr.y + 22.0f * k;

            // ---- ON THIS MACHINE, then the shared body --------------------------------
            cy = drawOnThisMachine(cdl, m, machine, model.receiverRunning, cx0, cy, colW, act, deck);
            cy += 22.0f * k;
            (void)st;
            ModulePageIn in;
            in.census = "fitted";
            in.id = m.catalogueId.empty() ? m.id : m.catalogueId;
            in.plate = &plate;
            in.pictures = cat != nullptr ? &cat->pictures : nullptr;
            in.catalogued = cat != nullptr;
            in.noticeBox = false;
            in.showReachLead = false;
            in.width = colW;
            const std::string glyphCat = kindGlyph(m);
            in.glyphCategory = glyphCat.c_str();
            ImGui::SetCursorScreenPos(ImVec2(cx0, cy));
            drawModulePageBody(in, deck.page, deck.pictures);
            cy = ImGui::GetCursorScreenPos().y;
        } else {
            // ================= THE LIST =============================================
            const std::vector<int> rows = fittedVisibleRows(model.modules, model.receiverRunning, deck);
            if (rows.empty()) {
                // WHAT AN EMPTY MODEL PROVES, AND WHAT IT DOES NOT. The host's scan only ever
                // collects files with the platform's module extension, so an empty list means
                // nothing in that folder was OFFERED to the loader - it does not mean the folder is
                // empty. A module the version policy retires is renamed aside so the scan stops
                // seeing it, and it is still a file sitting there.
                const char* why =
                    model.modules.empty()
                        ? tr("No module the host can load was found in the folder above, so nothing is "
                             "fitted. A module the version policy has retired is renamed aside and stops "
                             "being scanned - it is still a file in that folder, and the rail's Plugins "
                             "section lists it under Disabled.")
                        : tr("Every fitted module is hidden by the keys above. Press one to show it again.");
                cdl->AddText(uf, upx, ImVec2(cx0, cy), theme::kInkMuted, why, nullptr, cw);
                cy += wrapH(uf, upx, cw, why) + 12.0f * k;
            }
            const float stW = stateColumnWidth();
            const float stopW = stopKeyWidth();
            const float rmW = removeKeyWidth();
            const float rowGap = 8.0f * k;
            const float rightW = 14.0f * k + rmW + rowGap + stopW + 16.0f * k + stW + 14.0f * k;
            for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                const FittedModule& m = model.modules[static_cast<std::size_t>(rows[ri])];
                const FittedState st = fittedState(m, model.receiverRunning);
                const FittedRowNote note = fittedRowNote(m, model.receiverRunning);
                const float rowH = std::max(64.0f * k, 14.0f * k + faceH(uf, fonts::panelPx()) + 2.0f * k +
                                                           (note.text.empty() ? 0.0f : faceH(uf, upx)) + 14.0f * k);
                const ImVec2 rtl(cx0, cy), rbr(cx0 + cw, cy + rowH);
                cdl->AddRectFilled(rtl, rbr, theme::kEnamelDark);
                cdl->AddRect(rtl, rbr, ImGui::IsMouseHoveringRect(rtl, rbr) ? theme::kBrassMid
                                                                              : theme::withAlpha(theme::kBrassDark, 0.9f));
                ImGui::PushID(m.file.empty() ? "row" : m.file.c_str());
                // the glyph
                const float gb = 46.0f * k;
                const ImVec2 gtl(rtl.x + 14.0f * k, rtl.y + (rowH - gb) * 0.5f);
                ImGui::SetCursorScreenPos(gtl);
                bool open = ImGui::InvisibleButton("##glyph", ImVec2(gb, gb));
                const bool gHover = ImGui::IsItemHovered();
                cdl->AddRectFilled(gtl, ImVec2(gtl.x + gb, gtl.y + gb), theme::kVoid);
                cdl->AddRect(gtl, ImVec2(gtl.x + gb, gtl.y + gb), gHover ? theme::kPhosphor : theme::kBrassDark);
                drawCategoryGlyph(cdl, ImVec2(gtl.x + 1.0f, gtl.y + 1.0f), gb - 2.0f, kindGlyph(m), theme::kPhosphor,
                                  1.8f * (gb - 2.0f) / 56.0f);
                // the name and the version, the line under them
                const float tx = gtl.x + gb + 14.0f * k;
                const float tw = std::max(60.0f, rbr.x - rightW - tx);
                const std::string shown = m.loaded && !m.name.empty() ? m.name : m.file;
                const float npx = fonts::panelPx();
                const float nameH = faceH(uf, npx);
                const float ty = rtl.y + (rowH - nameH - (note.text.empty() ? 0.0f : 2.0f * k + faceH(uf, upx))) * 0.5f;
                const std::string cut = ellipsize(uf, npx, shown.c_str(), std::max(40.0f, tw - 60.0f * k));
                const float nw = textW(uf, npx, cut.c_str());
                ImGui::SetCursorScreenPos(ImVec2(tx, ty));
                if (ImGui::InvisibleButton("##name", ImVec2(std::max(8.0f, nw), nameH))) { open = true; }
                const bool nHover = ImGui::IsItemHovered();
                cdl->AddText(uf, npx, ImVec2(tx, ty), nHover || gHover ? theme::kPhosphor : theme::kIvory, cut.c_str());
                if (!m.version.empty()) {
                    ImFont* rf = fonts::ui();
                    const float rpx = fonts::tinyPx() * 1.15f;
                    cdl->AddText(rf, rpx, ImVec2(tx + nw + 12.0f * k, ty + (nameH - faceH(rf, rpx)) * 0.5f),
                                 theme::kAmber, m.version.c_str());
                }
                if (!note.text.empty()) {
                    addEllipsized(cdl, uf, upx, ImVec2(tx, ty + nameH + 2.0f * k),
                                  note.warning ? theme::kAmber : theme::kInkMuted, note.text.c_str(), tw);
                }
                if (open) { deck.pageFile = m.file; }
                // the right cluster: the state with its lamp, STOP or START, REMOVE
                float rx = rbr.x - 14.0f * k;
                const bool armed = deck.confirmRemove == m.file;
                {
                    const float kh = outlineKeyHeight();
                    const float ky = rtl.y + (rowH - kh) * 0.5f;
                    KeyRect rem;
                    const char* remLabel = m.orphaned ? tr("REMOVE FILE") : tr("REMOVE");
                    if (drawOutlineKey(cdl, ImVec2(rx - rmW, ky), ImVec2(rx, ky + kh), remLabel, theme::kInkMuted, true,
                                       "remove", tr(kKeptText), &rem)) {
                        // TWO-STEP REMOVE. Deleting a module deletes a file the user downloaded and may not be
                        // able to get back, so a single mis-click must not do it: REMOVE arms, CONFIRM acts,
                        // and the arming lapses after five seconds.
                        if (armed) {
                            deck.confirmRemove.clear();
                        } else {
                            deck.confirmRemove = m.file;
                            deck.confirmUntil = now + 5.0;
                        }
                    }
                    censusRect("fitted:row:" + m.id + ":remove", rem.tl.x, rem.tl.y, rem.br.x, rem.br.y);
                    rx -= rmW + rowGap;
                    // STOP / START - or, while REMOVE is armed, CONFIRM in its place, beside REMOVE.
                    KeyRect sk;
                    bool haveKey = false;
                    if (armed) {
                        if (drawOutlineKey(cdl, ImVec2(rx - stopW, ky), ImVec2(rx, ky + kh), tr("CONFIRM"),
                                           theme::kAlarmHot, true, "confirm", nullptr, &sk)) {
                            act.kind = m.orphaned ? FittedModulesAction::Kind::RemoveOrphan
                                                  : FittedModulesAction::Kind::Remove;
                            act.file = m.file;
                            deck.confirmRemove.clear();
                        }
                        haveKey = true;
                        censusRect("fitted:row:" + m.id + ":confirm", sk.tl.x, sk.tl.y, sk.br.x, sk.br.y);
                    } else if (m.loaded) {
                        // THE LABEL IS THE ACTION, never the state: a key saying "RUNNING" leaves the user
                        // guessing whether pressing it stops the module or is simply a badge.
                        // (0.99.73) START for an IDLE module as well as a STOPPED one: pressing it pins the
                        // module to ALWAYS, the same thing a preset press or the page's tick does.
                        const bool offersStart = fittedOffersStart(m);
                        if (drawChassisKey(cdl, ImVec2(rx - stopW, ky), ImVec2(rx, ky + kh),
                                           fittedKeyWord(m), true, "stop", &sk)) {
                            act.kind = offersStart ? FittedModulesAction::Kind::Start : FittedModulesAction::Kind::Stop;
                            act.file = m.file;
                        }
                        haveKey = true;
                    }
                    if (haveKey && !armed) {
                        censusRect("fitted:row:" + m.id + ":key", sk.tl.x, sk.tl.y, sk.br.x, sk.br.y);
                    } else if (armed) {
                        censusRect("fitted:row:" + m.id + ":key", sk.tl.x, sk.tl.y, sk.br.x, sk.br.y);
                    }
                    rx -= stopW + 16.0f * k;
                    // The lamp and the state word: both drawn whatever the colour says, because a lamp whose
                    // meaning is carried by colour alone cannot be read in a greyscale screenshot and is
                    // unreadable to about one man in twelve.
                    const char* word = fittedStateWord(st);
                    ImFont* lf = fonts::legend();
                    const float wpx = fonts::tinyPx();
                    const float wx = rx - stW + 22.0f * k;
                    drawBenchLamp(cdl, ImVec2(wx - 12.0f * k, rtl.y + rowH * 0.5f), 4.5f * k, stateLamp(st),
                                  stateLampLit(st), nullptr);
                    addTrackedText(cdl, lf, wpx, ImVec2(wx, rtl.y + rowH * 0.5f - faceH(lf, wpx) * 0.5f), stateInk(st),
                                   word, wpx * 0.10f, rx);
                }
                if (census::enabled()) {
                    census::rect("fitted:row:" + m.id, rtl.x, rtl.y, rbr.x, rbr.y);
                    census::rect("fitted:row:" + m.id + ":name", tx, ty, tx + nw, ty + nameH);
                    census::note("fitted:state:", m.id + ":" + censusStateName(st));
                }
                ImGui::PopID();
                cy += rowH + 8.0f * k;
            }
            // ---- CLEAN UP OLD VERSIONS, at the foot ----------------------------------------
            cy += 6.0f * k;
            ImGui::SetCursorScreenPos(ImVec2(cx0, cy));
            if (drawCleanupFoot(model.oldCopies, model.cleanupReport, false, cw, "fitted:cleanup",
                                "fitted:cleanupyes")) {
                act.kind = FittedModulesAction::Kind::CleanUp;
            }
            cy = ImGui::GetCursorScreenPos().y + 8.0f * k;
        }
        ImGui::SetCursorScreenPos(ImVec2(cx0, cy + 8.0f * k));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    ImGui::EndChild();

    // Esc returns from a page.
    if (onPage && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        deck.pageFile.clear();
        if (census::enabled()) { census::note("fitted:page:closed:", "esc"); }
    }
    // An armed delete follows its module and nothing else: with the list gone (a page, a
    // filter that hides it) there is nothing to confirm it against.
    if (onPage) { deck.confirmRemove.clear(); }

    ImGui::PopID();
    return act;
}

}  // namespace cascade::gui
