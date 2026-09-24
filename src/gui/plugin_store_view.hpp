// plugin_store_view.hpp - the PLUGIN STORE window's content: the catalogue.
//
// ONE FUNCTION, ONE WINDOW. The design handoff's principle, and the same one
// the satellites map already follows: everything for choosing a module is in
// this window, and the rail row that opens it is only the key. What this
// window is NOT is the operating panel - running or stopped, start, stop,
// remove, and why a fitted module was refused all belong to the FITTED MODULES
// window beside it. This one answers "what could I have"; that one answers
// "what do I have, and is it working". They share the DATA PLATE below and
// nothing else, so a module reads identically in both.
//
// ---------------------------------------------------------------------------
// THE ONE CLAIM THIS FILE REFUSES TO MAKE, and it is a safety matter.
//
// The design's data plate says of the reach list: "enforced by the console - a
// module cannot take anything not on this list", and "a module that asks for
// anything outside this list is refused at the point it asks". That is FALSE
// of this product and it is not drawn.
//
// Plugins are loaded IN-PROCESS: LoadLibraryExW on Windows
// (src/core/plugin_host.cpp:98), dlopen on POSIX (:130). There is no sandbox,
// no permission model and no out-of-process host. The CASCADE_CAP_* bits
// describe what a module PROVIDES - a decoder, a basemap, a panel, a preset -
// and are not a limit on what it may take. A fitted module runs with every
// privilege this application has.
//
// So the reach panel is KEPT, because stating reach in plain words before the
// fit key is the design's best idea, and its CLAIM is corrected: declared by
// the maker, not enforced. The one thing that IS enforced is named as such -
// the per-module tune grant, which PluginUi refuses without (see plugin_ui.hpp
// and CASCADE_TUNE_DENIED) - and nothing else is dressed up as a guarantee.
//
// Printing the design's sentence would hand the user a guarantee the product
// does not provide, on the very card - unverified maker, no licence stated -
// where they would lean on it hardest.
//
// THE SAME RULE APPLIES TO THE DOWNLOAD, and it caught this file out once.
// The updates banner said each key "checks its signature". NOTHING IN THIS
// PRODUCT VERIFIES A SIGNATURE. What PluginRepo::install actually does is
// worth stating and is stated - https only with the platform's certificate
// checks on, no cross-host redirect, a hard byte cap, an untrusted-file-name
// sanitiser, an exact ABI match, and a mandatory sha256 that the streamed
// bytes must match before the temp file is renamed into the plugins directory
// - but that digest is published by the same catalogue as the file, so it
// proves the bytes arrived unaltered and vouches for nobody. Saying
// "signature" would promise a second party who does not exist.
// ---------------------------------------------------------------------------
//
// WHAT IT CANNOT COMPUTE IS AN INPUT. This view owns no catalogue, no plugin
// host and no network. Everything it draws arrives in PluginStoreModel, filled
// by the wiring from the sources named against each field, so a figure on the
// panel can always be traced back to something the application measured.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGIN_STORE_VIEW_HPP
#define CASCADE_GUI_PLUGIN_STORE_VIEW_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "gui/text_fit.hpp"
#include "imgui.h"

namespace cascade::gui {

// ===========================================================================
// THE DATA PLATE - the component SHARED with the FITTED MODULES window
// ===========================================================================
//
// It lives here, in the store's files, and the fitted-modules window includes
// this header for it. One implementation, so the same module reads identically
// in both windows: the same facts in the same order, the same words for a
// missing one, and the same reach panel with the same corrected claim.
//
// IT KNOWS NOTHING ABOUT EITHER WINDOW. No catalogue, no filter, no selection,
// no store-only state - only a plain struct of what one module IS. That is
// what lets the other window pass a module it built from PluginHost records
// with no catalogue in sight.
//
// EVERY OPTIONAL FIELD IS ABSENT-BY-DEFAULT AND SAYS SO WHEN DRAWN. "0 bytes"
// and "we were never told the size" are opposite statements, and this product
// has been bitten by exactly that conflation before (see the no-reading rule
// at the top of scope_face.hpp). So a value that has no source is drawn
// hatched with the reason beside it, never as a clean zero.
struct ModulePlate {
    // --- identity ----------------------------------------------------------
    // From PluginCatalogEntry (name/version/author/licence/summary/
    // description/homepage/legalNotice) for a catalogue row, or from
    // LoadedPlugin (name/version/author/licence) for one that is fitted.
    //
    // WAS THERE A RECORD TO COPY THESE FROM AT ALL? A catalogue row always
    // has one, so this is TRUE by default and the caller only ever turns it
    // off. A FITTED module may not: PluginHost copies name, version, author
    // and licence out of the descriptor only AFTER validatePluginDesc accepts
    // it (plugin_host.cpp:232-249), so a file refused before that point - the
    // wrong ABI, no entry point, a capability with no table - reaches this
    // struct with all four empty. Drawing them as "not stated" and "none
    // declared" would be three inventions about a module nobody has read:
    // "the maker did not say" and "we never got as far as asking" are
    // opposite claims, and only the second one is true. FALSE makes the plate
    // letter those cells "not read" instead.
    //
    // It is NOT the same question as `loaded`. The host's duplicate resolver
    // turns a loaded module off AFTER reading it (plugin_host.cpp:826), so
    // that record is not loaded and its identity is perfectly well known.
    bool haveDescriptor = true;

    std::string name;
    std::string version;
    std::string maker;    // EMPTY means the record states no author. Drawn as
                          // "not stated" - never invented, never blanked.
                          // Meaningless unless haveDescriptor.
    std::string licence;  // EMPTY means none declared. The host refuses to
                          // LOAD a module with no licence, so this is a fact
                          // worth its own line rather than a shrug - but only
                          // when haveDescriptor says a licence was looked for.
    std::string blurb;    // summary, or description when there is one

    // THE ONE-LINE SUMMARY, KEPT APART FROM THE DESCRIPTION, and the reason is
    // a list nobody could read. `blurb` carries whichever of the two the
    // catalogue gave, which in the live index means the DESCRIPTION - 528 to
    // 2979 characters of it - and a row that wraps all of that is eleven lines
    // tall, so one module filled the whole list and the other twenty-three
    // were a scroll away. The summaries in the same index run 47 to 106
    // characters: one or two lines, which is a row.
    //
    // EMPTY IS NORMAL and falls back to `blurb`, because a catalogue that
    // states only a description is a catalogue this window still has to draw.
    // Nothing is ever cut to make it fit - the row wraps what it is given.
    std::string summary;

    std::string homepage;
    std::string legalNotice;  // shown verbatim; the acknowledgement gate is
                              // the store's, not the plate's

    // Bare file name in the plugins directory. Empty when the module is not
    // installed here - which is the normal case for a catalogue row.
    std::string fileName;

    // --- state on THIS machine ---------------------------------------------
    bool fitted = false;   // installed: a host record or a manifest row exists
    bool loaded = false;   // mapped and validated right now (LoadedPlugin::loaded)
    bool running = false;  // loaded AND not in the stop set (PluginUi::isStopped)

    // LoadedPlugin::error, verbatim - empty iff loaded. This is the refusal
    // reason, and it is the whole reason a fitted module can be silent.
    std::string refusalReason;

    // --- what the module declares ------------------------------------------
    // OR of CASCADE_CAP_* bits, from LoadedPlugin::capabilities.
    //
    // `haveCapabilities` is FALSE for a catalogue row, and that is not an
    // oversight to be papered over: PluginCatalogEntry carries no capability
    // field, so what a module declares is genuinely unknown until it is
    // fitted. The plate says that in words rather than drawing an empty list
    // that would read as "it declares nothing".
    //
    // IT IS ALSO FALSE FOR A REFUSED MODULE, for a different reason, and the
    // plate says which: a catalogue row has not been read yet, while a refused
    // file was read and rejected - it reaches nothing because it is not
    // loaded, not because it is harmless. An empty list drawn the same way for
    // both would report the second as the first.
    bool haveCapabilities = false;
    std::uint32_t capabilities = 0;

    // The per-module tune grant - the ONE permission this product actually
    // enforces. `haveTuneGrant` false means the grant was not looked up (a
    // catalogue row); it does not mean "denied".
    bool haveTuneGrant = false;
    bool tuneGranted = false;

    // --- the platform record -----------------------------------------------
    // PluginPlatform::sizeBytes for the build matching this host. Advisory in
    // the catalogue and advisory here. 0 with haveSizeBytes false means the
    // catalogue stated no size; there is no published date field anywhere in
    // the record, so no published date is drawn.
    bool haveSizeBytes = false;
    std::uint64_t sizeBytes = 0;

    // PluginCatalogEntry::abiVersion against CASCADE_PLUGIN_ABI_VERSION, or
    // InstalledPlugin::abiVersion for a fitted one. abiVersion 0 in a manifest
    // means "not recorded" and must be passed as haveAbi = false, never as a
    // mismatch - the same fail-open rule pluginBlockReason follows.
    bool haveAbi = false;
    std::uint32_t abiVersion = 0;
    std::uint32_t hostAbiVersion = 0;

    // "windows/x64, linux/x64" - the os/arch pairs the catalogue publishes a
    // build for. Empty when there is no catalogue record to read them from.
    std::string platforms;

    // PluginCatalogEntry::minSupportedVersion - the retirement floor. Empty is
    // the normal case and means NO floor; it must never be read as "retire
    // everything".
    std::string retirementFloor;
};

// The height the plate will take at `width`. Measured, not guessed, so a
// caller can size a column before drawing into it.
float moduleDataPlateHeight(float width, const ModulePlate& m);

// Draws the plate at `tl`, `width` wide. Returns the height consumed, which
// equals moduleDataPlateHeight(width, m).
//
// Draws only - it creates no ImGui items and raises no requests, so the two
// windows can put their own keys wherever their own layout wants them.
float drawModuleDataPlate(ImDrawList* dl, const ImVec2& tl, float width,
                          const ModulePlate& m);

// The KIND TAG - the small plate at the head of a module row.
//
// Derived from the declared capability bits, so it is a fact about the module
// rather than a category somebody typed. With no capability word it returns
// "NOT DECLARED" for a catalogue row, which genuinely does not say one yet,
// and "NOT KNOWN" for a fitted module the host would not accept - whose
// silence belongs to the refusal and not to the module.
const char* moduleKindTag(const ModulePlate& m);

// HOW WIDE THAT PLATE HAS TO BE, measured across EVERY word it can carry.
//
// IT WAS TWO NUMBERS, AND THAT IS THE FAULT THIS FIXES. The store's card used
// 84 px and the fitted-modules row used 82 minus 8, which is 74 - the same
// component, drawn ten pixels apart in the two windows the shared plate exists
// to keep identical. Both still HELD their words after the engraving grew
// ("NOT DECLARED", the longest, measures 46.8 px at 14), so this is a
// consistency fix rather than an overflow one; what it also buys is that the
// chip can never quietly stop holding them, because the tag is CENTRED in it
// and not clipped - a tag wider than its plate is not cut, it hangs out over
// the machined edge at both ends.
//
// And it is measured across every tag rather than the one on screen, because a
// chip that changed width with its word would move the module's name beside it
// from row to row.
//
// Call inside a frame: it asks the atlas to measure.
float moduleKindTagWidth();

// The one-line REACH SUMMARY for a row: what this module declares, in the
// fewest honest words. Never says "reaches nothing" - every plugin here is
// native code in this process, so nothing reaches nothing.
std::string moduleReachSummary(const ModulePlate& m);

// The colour that summary is drawn in, by the furthest thing the module
// declares: ivory-ink for a module that only produces output, gold for one
// that reaches outward (asks to move the receiver, or fetches from a server),
// faint for one whose declaration is unknown. NEVER rust - a declared
// capability is not a fault, and rust in this palette means trouble.
ImU32 moduleReachColour(const ModulePlate& m);

// --- a reason kept in English, drawn in the language in force -----------------
//
// WHY A MODULE CANNOT BE FITTED is one English sentence
// (AppWindow::pluginInstallBlockedReason), and it has to stay English where it
// is made: ADD ALL compares it ("already installed" is not a failure), the log
// records it, and the web page is handed it. So each reason is a FOX_TR_NOOP
// literal where it is defined, and translated only where it is DRAWN - the
// "Cannot fit:" line on a row and on the data plate, the red result line
// under them, the fitted window's copy of that line and the ADD ALL summary.
//
// Two of the reasons carry a value - the plugin ABI it was built for, the
// platform nobody built it for - so the English is made from a format string
// by the functions below, and trStoredReason() recognises a sentence made by
// either and formats the same values into that format's translation. The
// round trip is checked: a sentence that merely looks like one of them is
// drawn as it came.
std::string pluginAbiMismatchReason(unsigned builtFor, unsigned required);
std::string pluginNoBuildReason(const std::string& platform);  // "windows/x64"

// `english` in the language in force: its catalogue entry when it has one, a
// sentence from one of the two formats above re-made in its translation,
// otherwise `english` itself - so with English in force, and for words the
// host passes on verbatim (PluginRepo's sha256 and I/O errors), the text is
// byte for byte what it was.
std::string trStoredReason(const std::string& english);

// WHAT THE HOST KNOWS ABOUT THIS MODULE ON THIS MACHINE, in one word.
//
//   NOT FITTED        no file for it here
//   REFUSED           a file is here and the host rejected it
//   STOPPED           loaded, and the user stopped it
//   TAKES NO SIGNAL   loaded, started, and it declares no decoder - so
//                     nothing is ever routed to it, by design
//   STARTED           loaded and not stopped
//
// STARTED IS DELIBERATELY NOT "RUNNING". Whether a decoder is actually being
// fed depends on the runner's instance table and the receiver's own run state,
// and the plate is handed neither - it is a description of a module, not a
// meter. The FITTED MODULES window is handed both and splits this same module
// into FED and NOT FED; "started" is the coarser of the two answers and never
// the contradicting one, which is what lets the two windows sit side by side.
//
// The five words come from one function so that the row, the lamp beside it
// and the plate's own ON THIS MACHINE line cannot disagree.
const char* moduleStateWord(const ModulePlate& m);

// The ink that word is lettered in. Phosphor is reserved for something known
// to be working, which is a claim this side cannot make, so STARTED letters in
// plain ivory.
ImU32 moduleStateColour(const ModulePlate& m);

// Whether a lamp beside that word is LIT. Only REFUSED lights one: a panel of
// lit lamps means nothing, and this side cannot see the one state - being fed
// - that would earn a green light.
bool moduleStateLampLit(const ModulePlate& m);

// ===========================================================================
// THE STORE
// ===========================================================================

// One catalogue row, as the wiring supplies it.
struct StoreModule {
    // Everything the shared plate draws. `plate.fitted` is what the FITTED
    // rocker filters on, and it must be computed with the SAME test the
    // desktop already uses (AppWindow::catalogEntryInstalled), which compares
    // the sanitised file name against both the host's records and the
    // manifest - so a retired plugin still counts as fitted.
    ModulePlate plate;

    // PluginCatalogEntry::id - the key an update is planned against.
    std::string id;

    // IS THERE A BUILD THIS MACHINE COULD RUN? PluginCatalogEntry::compatible
    // (abiVersion exactly this host's) AND thisPlatform() != nullptr (an
    // os/arch build exists). A STABLE fact about the entry, which is why the
    // SHOW well sorts rows on it rather than on blockedReason: that reason
    // includes transient states such as "a transfer is already in progress",
    // and a filter that moved rows between categories while a download ran
    // would be a filter the user cannot trust.
    bool installableHere = false;

    // WHY FIT MAY NOT BE PRESSED, or empty when it may.
    //
    // MUST come from the SAME predicate the desktop's button uses
    // (AppWindow::pluginInstallBlockedReason), so the sentence under the key
    // and the key itself can never disagree. That predicate already covers a
    // transfer in flight, an ABI mismatch, no build for this host, no licence
    // declared, already installed, and an unacknowledged legal notice.
    std::string blockedReason;

    // THE SAME PREDICATE ASKED AS IF THE MAKER'S NOTICE HAD BEEN ACKNOWLEDGED,
    // and it exists for ADD ALL alone.
    //
    // `blockedReason` above is asked with the acknowledgement that belongs to
    // the SELECTED row and to no other, so every other module carrying a legal
    // notice reads "the legal notice must be acknowledged first" - seven of
    // the twenty-four in the live catalogue. An ADD ALL that silently passed
    // over seven modules would be a key whose word was a lie; one that
    // installed them regardless would be taking a consent nobody gave. So the
    // window is told BOTH answers, offers one tick that covers the notices,
    // and names the modules it is asking about.
    //
    // Identical to blockedReason for every module that has no notice.
    std::string blockedReasonIfAcknowledged;

    // From PluginRepo::planUpdates, when a plan exists for this id. Both empty
    // when none does - which is also the state before any catalogue has been
    // fetched, and the store says which of the two it is rather than printing
    // a clean zero.
    std::string updateToVersion;
    std::string updateReason;  // PluginUpdate::reason, verbatim
};

// Everything the store draws that it cannot work out for itself.
struct PluginStoreModel {
    std::vector<StoreModule> modules;

    // AppWindow::pluginCatalogueUrl_ - where the catalogue was read from. An
    // https:// index, or a path to a local index.json.
    std::string sourceUrl;

    // ARE THERE ROWS? This is AppWindow::catalog_ being non-empty and nothing
    // more, so it answers "is there a catalogue to show" and CANNOT answer
    // "has one ever been read" - a fetch that succeeded and returned an index
    // listing no plugins leaves it false, exactly like a fetch nobody ever
    // asked for. Those are different facts, and a window that reports the
    // first as the second sends the user to press CHECK NOW for ever.
    //
    // The window therefore never reads this alone: see the three states
    // below, which it derives from this and the two strings that follow.
    bool haveCatalogue = false;

    // AppWindow::catalogStatus_ / catalogError_, verbatim. A fetch failure is
    // the user's evidence and is never paraphrased.
    //
    // THEY ARE ALSO THE EVIDENCE THAT A FETCH HAPPENED AT ALL, which is what
    // separates the three states the window draws. AppWindow clears BOTH when
    // it starts a fetch, sets `sourceStatus` on every success ("N plugins in
    // the catalogue") and `sourceError` on every failure, so the pair always
    // describes the LAST completed attempt and nothing older:
    //
    //   both empty, no rows      nobody has asked. Nothing here is a count.
    //   status set, no rows      it was read, and it listed no modules.
    //   error only, no rows      it was asked and the attempt failed; the
    //                            reason is printed verbatim under CATALOGUE
    //                            SOURCE.
    //   rows                     it was read.
    //
    // Status is tested BEFORE error because a successful fetch can set both:
    // the catalogue loads and the version policy behind it fails to cache,
    // which is a read catalogue with a warning, not a failed check.
    std::string sourceStatus;
    std::string sourceError;

    // A fetch or a download is in flight (catalogPending_ || installPending_),
    // with PluginRepo::progress() and the name of what is moving. progress
    // stays at 0 when the server sends no Content-Length, and the bar then
    // simply does not move rather than inventing a figure.
    bool busy = false;
    float progress = 0.0f;
    std::string busyLabel;

    // AppWindow::installReport_ / installError_, verbatim. A sha256 mismatch
    // names both digests and must be shown exactly as PluginRepo wrote it.
    std::string resultReport;
    std::string resultError;

    // --- the ADD ALL run ----------------------------------------------------
    //
    // An ADD ALL is not one operation: it is N transfers through the single
    // install path, one after another, because PluginRepo applies exactly one
    // at a time. These three say where that run has got to, and the window
    // draws them instead of guessing from `busy`.
    //
    // `addAllProgress` is the line under the key while it runs - "installing 4
    // of 23: GOES Weather Satellites (HRIT / LRIT)" - and `addAllSummary` is
    // what is left on the panel when it ends: "23 installed, 0 failed", or the
    // names that failed with the reason each gave. Both empty means no run has
    // happened this session.
    bool addAllRunning = false;
    std::string addAllProgress;
    std::string addAllSummary;
    // True when the run ended with at least one failure, so the summary is
    // lettered as trouble rather than as a result.
    bool addAllFailed = false;
};

// ===========================================================================
// WHAT THE STORE SAYS ABOUT A MODULE ON THIS MACHINE, in one word
// ===========================================================================
//
// NOT moduleStateWord, and the two answer different questions. That one is
// about RUNNING - started, stopped, refused, fed nothing - and it is shared
// with the FITTED MODULES window, which is the window about running. This one
// is the CATALOGUE's question: is this module here, and is it current. A store
// that answers "STARTED" to "have I got this" is answering something else.
enum class StoreInstallState {
    NotInstalled,     // no file for it here, and one could be fetched
    CannotFit,        // no file here, and no build this machine could run
    Installed,        // here, loaded, and the catalogue offers nothing newer
    UpdateAvailable,  // here, and the catalogue offers a newer build
    Refused,          // here, and the host would not have it
};

StoreInstallState storeInstallState(const StoreModule& sm);

// The word itself: NOT INSTALLED, CANNOT FIT, INSTALLED, UPDATE, REFUSED.
// Drawn at the window's own prose size beside the key, not as a chip - "not
// only a small icon" was the whole complaint.
const char* storeInstallWord(StoreInstallState s);

// The ink it is lettered in. NEVER kAmber: amber in this palette is a READING,
// something the machine measured, and an install state is not a measurement.
// Only REFUSED takes the alarm ink - not being installed is not a fault.
ImU32 storeInstallColour(StoreInstallState s);

// THE SIZE THIS WINDOW SETS ITS PROSE IN, from the theme's own ladder.
//
// It was fonts::kTinySize - the smallest engraving in the application - for
// every sentence on the panel: the module summaries, the maker and licence
// line, the reach rows on the data plate, every note and every key's label.
// That is the right size for a word cut into a metal chip and the wrong one
// for the paragraph a user reads before deciding to install something, which
// is what the owner reported ("make the plugin store larger and easier to
// read"). This is a theme size and not a number invented here; the captions
// keep theirs.
float storeProsePx();

// THE SHOW WELL'S SIX ROCKERS keep their two columns in every language. The
// width one rocker row needs with its longest label lettered at `labelPx`
// (drawRockerRow's switch, plate padding and a three-figure count around it),
// and whether a column `colW` wide holds two columns: it does whenever the
// longest label fits at its FLOOR (seven tenths of storeProsePx), because
// drawRockerRow draws a label smaller before it lets it run past its plate.
// English fits at full size, so its well is exactly what it was.
float storeShowRockerMinWidth(float labelPx);
bool storeShowTwoColumns(float colW);

// THE CARD'S ACTION COLUMN: the key and, under it, the install word and the
// running-state word. Its width is measured from every word it can hold. A
// status word is ONE LINE, drawn smaller to fit the column (down to seven
// tenths of storeProsePx) and wrapped only when even that cannot hold it -
// never broken in the middle of the word the way "PAIGALDAMAT / A" was
// (et, 34-language review).
float storeActionColumnWidth();
float storeStatusWordRoom();
LineFit storeStatusWordFit(const char* word);

// THE LIST IS WHAT THIS WINDOW IS FOR, AND IT HAD NO FLOOR (the owner,
// 2026-09-18, on 0.99.2: "it's not letting me scroll on the plugin store to see
// the plugins"). Three bands sit above it - the ADD ALL well, the updates
// banner and the three-well control deck - and together they come to about
// 590 px at the page's engraving sizes. The store opens clamped inside the
// main window, which on a fresh install is 1282 x 745, so the store is about
// 1234 x 697 and the list was left 65 px: less than one module card, with the
// other twenty-three "a scroll away" in a pane nobody could see into. Shorter
// still, the list was pushed below the window's bottom edge entirely.
//
// This is the least height the BODY - the module list column and the data
// plate beside it - is ever given. The list column spends about 55 px of it on
// its own heading and rule, so the list itself keeps about 345 px: two whole
// module cards (about 130 px each with a one-line summary at these sizes) and
// part of a third. 340 was tried first and left the list 285 px.
inline constexpr float kStoreListMinH = 400.0f;

// THE FLAGS OF THE PANE THE WHOLE STORE IS DRAWN INTO, owned here so the
// window that draws it and the test that measures it cannot disagree. It was
// NoScrollbar | NoScrollWithMouse, on the grounds that the view always fits
// the pane it is given - which it did not, and a pane that cannot scroll turns
// "does not fit" into "is not there". It now scrolls, with a scrollbar that
// appears only when there is something below the edge: when the window is
// tall enough nothing changes, and when it is not, the list is still reachable.
ImGuiWindowFlags storeFaceWindowFlags();

// The control deck's settings. Owned by the CALLER because they outlive one
// frame and the caller may persist them; the view edits them in place and
// keeps no second copy.
struct PluginStoreDeck {
    // The search text, over name, maker and description. A fixed buffer
    // because it is handed straight to ImGui::InputText.
    char search[128] = {0};

    // The SHOW well. Two groups of three rockers; within a group the rows
    // shown are the OR of what is switched on, and a row must pass BOTH
    // groups. THE THREE CATEGORIES IN EACH GROUP ARE DISJOINT AND EXHAUSTIVE -
    // every module is in exactly one of each - which is what makes the
    // semantics readable without a legend, and makes "nothing switched on
    // shows nothing" a statement the well can safely make.
    bool showFitted = true;      // installed on this machine
    bool showAvailable = true;   // not installed, StoreModule::installableHere
    bool showBlocked = true;     // not installed, and no build this host can run
    bool showDecoders = true;    // declares a decoder capability
    bool showOtherKinds = true;  // declares something, but not a decoder
    bool showUndeclared = true;  // capabilities not known - every catalogue row

    // 0 NAME, 1 MAKER, 2 VERSION. See kStoreSortCount.
    int sortKey = 0;

    // Index into PluginStoreModel::modules, or -1 for nothing selected. The
    // view clamps it and re-clamps it when the catalogue changes underneath.
    int selected = -1;

    // The legal-notice acknowledgement, which belongs to ONE module. The view
    // clears it whenever the selection moves, so a tick given to the plugin
    // the user just read about is never carried over to the next one.
    bool legalAck = false;

    // THE ADD ALL ACKNOWLEDGEMENT, which is a DIFFERENT tick and deliberately
    // not the one above. It covers every module in the run that carries a
    // maker's notice, the window names them beside it, and it is not persisted
    // anywhere - a consent that survived a restart would be a consent nobody
    // remembers giving.
    bool addAllAck = false;
};

// ===========================================================================
// ADD ALL - the whole decision, in one pure function
// ===========================================================================
//
// WHAT IT PICKS AND WHAT THE KEY SAYS, with no ImGui in it, so both can be
// checked against a model built in a test rather than against a screenshot.
// The window does nothing with this but draw it and, on a press, hand the two
// index lists back.
struct AddAllPlan {
    // Indices into PluginStoreModel::modules, in catalogue order. Every one is
    // a module whose own blockedReason was empty, so each goes through the
    // SAME gate a single FIT goes through - and is re-tested at the moment it
    // starts, because a plan made one frame is applied over many.
    std::vector<int> install;
    std::vector<int> update;

    // "NAME - reason", one per module the run will pass over. NAMED, because
    // "17 installed, 7 skipped" tells the user nothing they can act on.
    std::vector<std::string> skipped;

    // How many of `skipped` are held back by a maker's notice alone - the ones
    // the tick beside the key would add. Zero once it is ticked.
    int heldByNotice = 0;

    // The engraving on the key. "ADD ALL PLUGINS" when the run really is all
    // of them; otherwise the counts, so the word and the deed agree.
    std::string label;

    // Empty when the key may be pressed. A dead key ALWAYS says why - the rule
    // the rest of this window already follows.
    std::string blockedReason;
};

// `noticesAcknowledged` is PluginStoreDeck::addAllAck: it swaps each module's
// blockedReason for its blockedReasonIfAcknowledged, which is the same string
// for every module that carries no notice.
AddAllPlan planAddAll(const PluginStoreModel& model, bool noticesAcknowledged);

inline constexpr int kStoreSortCount = 3;

// The engraved word over sort key `index`. An index outside the range answers
// with the first key rather than with whatever the last case happened to be.
const char* storeSortLabel(int index);

class PluginStoreView {
public:
    // Draws the whole window's content into the CURRENT ImGui window, filling
    // `width` x `height`. `model` is borrowed for the call only.
    void draw(float width, float height, const PluginStoreModel& model,
              PluginStoreDeck& deck);

    // --- what the last draw() asked for ------------------------------------
    //
    // Requests rather than callbacks, for the reason MapView raises its own:
    // the caller is the only object that knows about the plugin host, the
    // repository and the worker threads, and it applies them AFTER the frame
    // rather than from inside a draw. All three are cleared at the start of
    // every draw(), so a request is answered once or not at all.

    // CHECK NOW was pressed: fetch the catalogue at model.sourceUrl.
    bool checkNowRequested() const { return checkNow_; }

    // CANCEL was pressed during a transfer.
    bool cancelRequested() const { return cancel_; }

    // FIT was pressed on this index into model.modules, or -1. The view only
    // offers it where StoreModule::blockedReason is empty, but the caller must
    // still re-test: the predicate can have changed between the frame that
    // drew the key and the frame that handles it.
    int fitRequested() const { return fitIndex_; }

    // UPDATE was pressed on this index into model.modules, or -1. One module
    // per press, deliberately: PluginRepo has a single progress/cancel pair
    // and applies exactly one transfer at a time (see planUpdates' note that
    // there is no bulk and no automatic caller), so there is no key here that
    // fits several at once.
    int updateRequested() const { return updateIndex_; }

    // ADD ALL PLUGINS was pressed. The caller re-plans from its own state
    // rather than trusting the plan the key was drawn from - one frame's plan
    // applied over a run of transfers is exactly the thing that goes stale.
    bool addAllRequested() const { return addAll_; }

private:
    bool checkNow_ = false;
    bool cancel_ = false;
    bool addAll_ = false;
    int fitIndex_ = -1;
    int updateIndex_ = -1;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_STORE_VIEW_HPP
