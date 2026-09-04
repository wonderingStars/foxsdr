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
};

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
};

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

private:
    bool checkNow_ = false;
    bool cancel_ = false;
    int fitIndex_ = -1;
    int updateIndex_ = -1;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_STORE_VIEW_HPP
