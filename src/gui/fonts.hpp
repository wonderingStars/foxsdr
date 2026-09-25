// fonts.hpp - the three typefaces the bench is lettered in.
//
// WHY THIS EXISTS. Until it did, FoxSDR drew every word in ProggyClean, the
// 13-pixel bitmap face Dear ImGui falls back to when an application supplies
// none. That face is a fine default and completely wrong for this product: it
// is a 2000s programmer's terminal font sitting inside a 1960s instrument, it
// has one weight, and its digits are the same width and colour as its letters
// so a READING and a LEGEND look identical. The design handoff names its
// typefaces explicitly, and this is where they arrive.
//
// THE THREE ROLES, and they map onto the palette's rule one for one:
//
//   ui()       Saira Condensed Medium.  Everything a hand operates, and all
//              prose. Condensed because a bench panel is a crowded thing and
//              the design is drawn at 9-11px in a scaled artboard; Medium
//              rather than the design's Regular because at real size on dark
//              enamel a condensed 400 goes thin and grey, and a legend a user
//              squints at is a worse fault than one a shade too heavy.
//
//   legend()   Saira Condensed SemiBold. The engraved captions, section
//              plates and the maker's plate - the design's own weight 600.
//
//   reading()  Nova Mono. DIGITS, and very nearly nothing but. A counter's
//              figures sit on glass in a monospaced face so they stop
//              jittering sideways as they change, which is the single most
//              visible difference between an instrument and a form.
//
//              THE NARROWNESS OF THAT RULE IS MEASURED, NOT FASTIDIOUS. Nova
//              Mono draws its capital M as three close stems in a monospaced
//              cell; below about 20px they merge and the letter rasterises as
//              a solid block. "MUTED" in a status card came out as a filled
//              rectangle followed by UTED. So: figures take this face, words
//              take ui() whatever they are reporting, and a value carrying
//              units - "2.000 MS/s" - is a word for this purpose. The amber
//              of a reading is what carries the meaning; the monospacing is
//              only there to stop digits dancing.
//
// SIZES LIVE HERE, not at the call sites. A panel where three captions are
// 14px and a fourth is 15 because someone typed it twice is exactly the drift
// the theme file was written to stop.
//
// Dear ImGui 1.92 bakes a face at whatever size it is asked for, so each
// typeface is added to the atlas ONCE and drawn at any size through
// PushFont(font, size). There is no atlas cost to a new size, only to a new
// face.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_FONTS_HPP
#define CASCADE_GUI_FONTS_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "imgui.h"

namespace cascade::gui::fonts {

// --- the sizes, by role ------------------------------------------------------
// Measured against the design handoff and then checked at 100% Windows scaling
// against the widths the layout already hard-codes.
//
// RAISED BY TWO POINTS THROUGHOUT, on the report that captions were hard to
// read. The design is drawn at 9-11 px in a scaled artboard and those figures
// were carried across too literally: on a real panel at 100% scaling they are
// small, and the engraved tones make them smaller still, because a caption cut
// dark into brass is about 2.3:1 contrast and is carrying most of the loss.
// Size is the half of that this file controls.
//
// EVERY HARD-CODED WIDTH IN THE INTERFACE WAS MEASURED AGAINST THE OLD NUMBERS.
// Changing them here is one edit and a sweep: keys, plates, chips, drum
// apertures, meter faces and card heights all have to be re-checked, because
// text that no longer fits does not wrap, it clips. Do not raise these again
// without walking the same surfaces.
//
// RAISED BY THREE PIXELS ACROSS THE BOARD in 0.79.0, at the user's request
// ("make the font size larger across the whole software by about 3px"),
// which was the third such request in a month and the largest. The sweep
// above was walked again: the rail, the status cards, the plates, the bench
// engravings and the scope's panels were all screenshotted at the new sizes.
//
// BROUGHT BACK DOWN IN 0.84.0, WITH GEORGIA. The three-pixel raise above was
// made for a condensed sans; a serif at the same pixel size is a much
// larger-looking thing, wider by half again and heavier on the page, and the
// first sight of it at 21 px drew "drop the size down, it's too big". These
// are the pre-0.79.0 figures, which on Georgia read about as large as the
// raised ones did on Saira.
inline constexpr float kUiSize = 17.0f;       // controls, prose, table cells
inline constexpr float kLegendSize = 15.0f;   // engraved captions, small plates
inline constexpr float kReadingSize = 16.0f;  // a number on glass
inline constexpr float kTinySize = 14.0f;     // the smallest engraving that
                                              // still has to be readable

// THE LARGEST ENGRAVING IN THE APPLICATION, and it is deliberately used by ONE
// surface: a PAGE whose entire content is prose a user reads before deciding
// to put somebody else's native code into this process. The plugin store is
// that page - its rows are module names, summaries, licences and the list of
// what each module reaches for - and at kTinySize, which is what every one of
// those sentences used to be set in, the owner's report was "make the plugin
// store larger and easier to read", then "go bigger on the font".
//
// IT IS A NEW SIZE RATHER THAN A RAISE OF THE FOUR ABOVE, and that is the
// whole point of adding it. The sweep this file's header warns about - keys,
// plates, chips, drum apertures, meter faces and card heights all measured
// against the old numbers - is what a raise of kUiSize costs, and 0.79.0 paid
// it (kMenuWidth 268, axis pitch 104) for a change 0.84.0 then took back. A
// size nothing else reads cannot move the rail, the spectrum axis or a meter
// face, because none of them can see it.
//
// 21 px is the pre-0.84.0 kUiSize - the figure this application ran at for
// five releases - so it is a proven size on Georgia rather than a guess.
inline constexpr float kPanelSize = 21.0f;

// --- the faces ---------------------------------------------------------------
//
// NONE OF THESE EVER RETURNS NULL. Before load() has run, or after it failed,
// each returns whatever face is currently bound - so a missing typeface
// degrades to the wrong typeface and never to a crash. That matters more than
// it looks: ImFont::CalcTextSizeA is how half the instrument faces in this
// application position their own text, it takes no null, and a guard at every
// one of those call sites is a guard that will eventually be forgotten at one.
//
// Call them inside a frame. They ask ImGui for the current font on the
// fallback path, which needs a context.
ImFont* ui();
ImFont* legend();
ImFont* reading();

// Adds all three faces to the current atlas and makes ui() the default.
//
// Call once, after ImGui::CreateContext and before the first frame. Returns
// false if the atlas refused a face; the application is still perfectly usable
// in that case, wearing ImGui's own font, and callers should say so rather
// than abort.
bool load();

// --- Georgia, from the operating system (0.84.0) ----------------------------
//
// THE UI AND LEGEND ROLES ARE LETTERED IN GEORGIA when the machine has it,
// which every Windows does: Georgia Regular for everything a hand operates
// and for prose, Georgia Bold for the engraved captions. It is read from the
// system's font directory at start-up and is NOT in the binary, because
// Georgia is Microsoft's and licensed with Windows rather than for
// redistribution - the embedded OFL faces above stay in the binary as the
// fallback for a desktop without it (Linux, or a stripped Windows), and the
// two roles fall back TOGETHER so no window ever mixes the pairs. Figures
// keep Nova Mono: Georgia's numerals are old-style and proportional, and a
// counter whose digits dance and dip is the one thing an instrument's glass
// must never do.
//
// systemFontPath: where the named file would be on this platform - the
// Windows font directory (honouring %WINDIR%), or empty where there is no
// such convention. Pure, so a test can pin it without an atlas.
std::string systemFontPath(const char* file);

// True after load() when both Georgia faces were found and are in use; false
// on the embedded fallback. The diagnostics log says which at start-up.
bool usingSystemSerif();

// --- The face CHAIN: every script the catalogues are written in --------------
//
// Each of the three roles is not one typeface but a CHAIN of them, merged into
// one ImFont (ImFontConfig::MergeMode): Dear ImGui 1.92 asks each source in
// order for a glyph and takes the first that has it. The chain, in order:
//
//   1. THE PAIR the language is lettered in - Georgia where the machine has
//      it, else the embedded Saira Condensed, else the embedded Noto Sans
//      Condensed: the FIRST of those whose two faces have every letter of the
//      catalogue in force (CJK aside). English, and every Latin catalogue
//      Georgia covers, therefore looks exactly as it did; Vietnamese, whose
//      stacked vowels Georgia lacks, is lettered wholly in Saira; Russian on
//      Linux, where Saira has no Cyrillic, wholly in Noto Sans - never a word
//      assembled from two typefaces. (Figures keep Nova Mono in every case.)
//   2. NOTO SANS CONDENSED, compiled in (third_party/fonts, an OFL subset:
//      Latin-1, Latin Extended-A/B and Additional, Greek, Cyrillic), for the
//      odd letter the pair lacks - a country or language name in another
//      alphabet, a Bulgarian ѝ Georgia does not have.
//   3. A SYSTEM CJK FACE, only when a Chinese, Japanese or Korean catalogue is
//      in force (or the language list is on screen and needs one to draw a
//      language's own name). Read from the operating system and never
//      shipped - see core/scripts.hpp - and loaded LAZILY, so a user who never
//      asks for one of those languages carries none of their megabytes.
//
// EVERY FACE IN A CHAIN IS DRAWN AT THE EM OF THE FACE ENGLISH IS LETTERED IN
// on this machine (Georgia, else Saira; Nova Mono for readings). ImGui sizes a
// face by its ascent-to-descent span, which differs by a third between these
// families, so without this a Cyrillic fallback letter sat visibly smaller than
// the Latin beside it, and a whole Russian interface would be lettered at a
// different size from the English one every width here was measured against.
//
// THE ATLAS CHANGES ONLY BETWEEN FRAMES: applyLanguage() and the names request
// are recorded, and applyPending() - called where the application applies a
// language, before NewFrame - rebuilds or extends the atlas. A change that
// only ADDS faces behind the ones in use extends it; one that changes the pair
// or which CJK face comes first (Japanese after Chinese must put Yu Gothic
// ahead of YaHei, or the Japanese reader gets Chinese glyph forms) rebuilds it.

// Everything the chain for one language is made of - what applyPending()
// loads, and what test_i18n_glyphs checks every catalogue against, so the
// test and the application cannot disagree about which faces there are.
struct ChainFace {
    std::string label;  // "Georgia", "Noto Sans Condensed Medium", "msyh.ttc#1"
    const unsigned char* data = nullptr;  // valid for the life of the process
    std::size_t len = 0;
    int index = 0;  // the face inside a collection
};
struct Chain {
    std::string pair;  // "Georgia", "Saira Condensed" or "Noto Sans Condensed"
    std::vector<ChainFace> ui, legend, reading;
    // The CJK face the language needs, and whether this machine has one. When
    // it does not, ui/legend/reading hold no CJK face and the language is not
    // drawable here.
    int cjk = 0;  // a core::scripts::CjkFace
    bool cjkFound = true;
};
Chain planChain(const std::string& languageCode);

// Whether `languageCode`'s catalogue can be drawn on this machine: always, for
// a language the compiled-in faces cover; for a CJK one, when a system face
// with its characters exists. Installed as i18n's drawable predicate by
// load(). Reads font files the first time a CJK language is asked about (the
// answer is kept; the bytes are not, unless the face is put in the atlas).
bool canDraw(const std::string& languageCode);

// Why not, as an English sentence that is also a translation key (draw it
// through tr()); "" when canDraw() is true.
const char* unavailableReason(const std::string& languageCode);

// Record that `languageCode` is now in force; applyPending() puts its chain
// in the atlas.
void applyLanguage(const std::string& languageCode);

// Record the typeface pair the interface THEME asks for ("" = the
// application's own order, "Saira Condensed", "Noto Sans Condensed" or
// "Georgia"); applyPending() rebuilds the atlas with it first in line. A pair
// that lacks a letter the catalogue in force needs is still passed over.
void setPreferredPair(const std::string& pair);

// Record that the language list is on screen: applyPending() adds, behind the
// chain in force, the system faces the drawable languages' own names need
// ("日本語", "한국어") - once, and only for names the chain cannot draw.
void requestLanguageNames();

// Between frames: make the atlas match what was recorded. Returns true when
// the atlas changed.
bool applyPending();

// For the log and the tests: the pair in force, and every face in the UI
// role's chain in order, by label (a names face marked " (names)").
const std::string& pairInUse();
std::vector<std::string> uiChainLabels();

}  // namespace cascade::gui::fonts

#endif  // CASCADE_GUI_FONTS_HPP
