// plugin_store_view.hpp - the PLUGIN STORE window's content: the catalogue, as a
// shop window (0.99.72).
//
// TWO TABS AND A PAGE. BROWSE is the catalogue in category sections of cards;
// UPDATES (n) is what the catalogue offers over what is fitted; a card's name or
// glyph opens the plugin's own PAGE - pictures, what it does, what is new, the
// legal notice it has to be read against, and the facts folded behind SHOW
// DETAILS. What this window is NOT is the operating panel: running or stopped,
// start, stop, remove, and why a fitted module was refused all belong to the
// FITTED MODULES window beside it (gui/plugins_view.hpp). This one answers "what
// could I have"; that one answers "what do I have, and is it working". They share
// the PLUGIN PAGE BODY below - one renderer, two callers - and nothing else, so a
// module's pictures, description and facts read identically in both.
//
// ---------------------------------------------------------------------------
// THE ONE CLAIM THIS FILE REFUSES TO MAKE, and it is a safety matter.
//
// A design for a store of native code says of the reach list: "enforced by the
// console - a module cannot take anything not on this list". That is FALSE of
// this product and it is not drawn. Plugins are loaded IN-PROCESS: LoadLibraryExW
// on Windows (src/core/plugin_host.cpp), dlopen on POSIX. There is no sandbox, no
// permission model and no out-of-process host. The CASCADE_CAP_* bits describe
// what a module PROVIDES - a decoder, a basemap, a panel, a preset - and are not
// a limit on what it may take. A fitted module runs with every privilege this
// application has.
//
// So REACHES is stated in plain words, and its CLAIM is "declared by the maker,
// not enforced" (kReachLead in the .cpp). The one thing that IS enforced is named
// as such - the per-module tune and radio-settings grants, which PluginUi refuses
// without - and nothing else is dressed up as a guarantee.
//
// THE SAME RULE APPLIES TO THE DOWNLOAD. NOTHING IN THIS PRODUCT VERIFIES A
// SIGNATURE. What PluginRepo::install does - https only with certificate checks
// on, no cross-host redirect, a hard byte cap, an exact ABI match, and a
// mandatory sha256 the streamed bytes must match before the file is renamed into
// the plugins directory - proves the bytes arrived unaltered, and the digest is
// published by the same catalogue as the file, so it vouches for nobody. No copy
// in this window says "signature".
// ---------------------------------------------------------------------------
//
// WHAT IT CANNOT COMPUTE IS AN INPUT. This view owns no catalogue, no plugin
// host and no network. Everything it draws arrives in PluginStoreModel, filled by
// AppWindow::buildPluginStoreModel from the sources named against each field, so
// a figure on the page can always be traced back to something the application
// measured. What the view asks for goes back as REQUESTS the caller applies after
// the frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGIN_STORE_VIEW_HPP
#define CASCADE_GUI_PLUGIN_STORE_VIEW_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/plugin_cleanup.hpp"
#include "gui/text_fit.hpp"
#include "imgui.h"

namespace cascade::gui {

// ===========================================================================
// ONE MODULE'S FACTS - shared by the store page and the fitted modules page
// ===========================================================================
//
// IT KNOWS NOTHING ABOUT EITHER WINDOW. No catalogue, no filter, no selection,
// no store-only state: a plain struct of what one module IS, so the other window
// can pass a module it built from PluginHost records with no catalogue in sight.
//
// EVERY OPTIONAL FIELD IS ABSENT-BY-DEFAULT AND SAYS SO WHEN DRAWN. "0 bytes" and
// "we were never told the size" are opposite statements, and this product has
// been bitten by exactly that conflation before (see the no-reading rule at the
// top of scope_face.hpp). A value with no source is drawn hatched with the reason
// beside it, never as a clean zero.
struct ModulePlate {
    // --- identity ----------------------------------------------------------
    // WAS THERE A RECORD TO COPY THESE FROM AT ALL? A catalogue row always has
    // one, so this is TRUE by default and the caller only ever turns it off. A
    // FITTED module may not: PluginHost copies name, version, author and licence
    // out of the descriptor only AFTER validatePluginDesc accepts it, so a file
    // refused before that point reaches this struct with all four empty. Drawing
    // them as "not stated" would be three inventions about a module nobody has
    // read: FALSE makes the page letter those cells "not read" instead. It is NOT
    // the same question as `loaded`: the duplicate resolver turns a loaded module
    // off AFTER reading it, so that record is not loaded and its identity is
    // perfectly well known.
    bool haveDescriptor = true;

    std::string name;
    std::string version;
    std::string maker;    // EMPTY means the record states no author
    std::string licence;  // EMPTY means none declared (only meaningful when haveDescriptor)
    std::string blurb;    // the description, or the summary when there is none:
                          // WHAT IT DOES

    // THE ONE-LINE SUMMARY, KEPT APART FROM THE DESCRIPTION: the live index's
    // descriptions run to three thousand characters and its summaries to about a
    // hundred. A card is one line; the page is the paragraph.
    std::string summary;

    std::string homepage;
    std::string legalNotice;  // shown verbatim; the acknowledgement gate is the store's
    std::string originNote;   // ONE MUTED LINE for a regional plugin; empty draws nothing

    // Bare file name in the plugins directory. Empty when not installed here.
    std::string fileName;

    // --- state on THIS machine ---------------------------------------------
    bool fitted = false;   // a host record or an install record exists
    bool loaded = false;   // mapped and validated right now
    bool running = false;  // loaded AND not stopped
    std::string refusalReason;  // LoadedPlugin::error, verbatim - empty iff loaded

    // --- what the module declares ------------------------------------------
    // OR of CASCADE_CAP_* bits. For a FITTED module they are the descriptor's;
    // for a catalogue row, what the catalogue says the binary declares
    // (PluginCatalogEntry::capabilities) - and `haveCapabilities` is FALSE when
    // the catalogue says nothing, which is "not declared", not "declares none".
    // IT IS ALSO FALSE FOR A REFUSED MODULE, for a different reason: it was read
    // and rejected, so it reaches nothing because it is not loaded.
    bool haveCapabilities = false;
    std::uint32_t capabilities = 0;

    // The per-module tune grant - the ONE permission this product actually
    // enforces on this side. `haveTuneGrant` false means it was not looked up.
    bool haveTuneGrant = false;
    bool tuneGranted = false;

    // --- the platform record -----------------------------------------------
    bool haveSizeBytes = false;
    std::uint64_t sizeBytes = 0;

    // PluginCatalogEntry::abiVersion against CASCADE_PLUGIN_ABI_VERSION, or the
    // install record's for a fitted one. abiVersion 0 in a manifest means "not
    // recorded" and must be passed as haveAbi = false, never as a mismatch.
    bool haveAbi = false;
    std::uint32_t abiVersion = 0;
    std::uint32_t hostAbiVersion = 0;

    // "windows/x64, linux/x64" - the os/arch pairs the catalogue publishes a
    // build for. Empty when there is no catalogue record to read them from.
    std::string platforms;

    // PluginCatalogEntry::minSupportedVersion - the retirement floor. Empty is the
    // normal case and means NO floor.
    std::string retirementFloor;

    // --- the shop-window fields (0.99.72) ----------------------------------
    std::string category;   // catalogue id: "aircraft", "marine", ... or "" / unknown
    bool experimental = false;
    std::string whatsNew;   // one short paragraph about THIS version, or empty
    std::string published;  // "YYYY-MM-DD" or empty
    std::string sha256;     // the digest of THIS host's build, or empty
};

// The reach words of a module (today's, from the descriptor or catalogue bits).
// One row per thing it declares: `key` the short name, `detail` the sentence
// under it, `outward` set for a capability that reaches beyond the host.
struct ReachRow {
    std::string key;
    std::string detail;
    bool outward = false;
};
std::vector<ReachRow> moduleReachRows(const ModulePlate& m);

// The reach row names joined for one line ("Audio decoder, Map targets"), or the
// words for "not declared" / "not known" when there are none to say.
std::string moduleReachesLine(const ModulePlate& m);

// The one-line REACH SUMMARY for a fitted row: what this module declares, in the
// fewest honest words. Never says "reaches nothing" - every plugin here is native
// code in this process.
std::string moduleReachSummary(const ModulePlate& m);

// The colour that summary is drawn in, by the furthest thing the module declares.
// NEVER rust - a declared capability is not a fault.
ImU32 moduleReachColour(const ModulePlate& m);

// "fitted and started", "not fitted", ... : the ON THIS MACHINE fact, from the
// same rule everywhere. Never says RUNNING: whether anything reaches the module is
// the FITTED MODULES window's answer, and it is handed the runner.
std::string moduleMachineText(const ModulePlate& m);

// --- a reason kept in English, drawn in the language in force -----------------
//
// WHY A MODULE CANNOT BE FITTED is one English sentence
// (AppWindow::pluginInstallBlockedReason), and it has to stay English where it is
// made: ADD ALL compares it ("already installed" is not a failure), the log
// records it, and the web page is handed it. So each reason is a FOX_TR_NOOP
// literal where it is defined, and translated only where it is DRAWN. Two of the
// reasons carry a value - the plugin ABI it was built for, the platform nobody
// built it for - so the English is made from a format string by the functions
// below, and trStoredReason() recognises a sentence made by either and formats the
// same values into that format's translation.
std::string pluginAbiMismatchReason(unsigned builtFor, unsigned required);
std::string pluginNoBuildReason(const std::string& platform);  // "windows/x64"

// `english` in the language in force: its catalogue entry when it has one, a
// sentence from one of the two formats above re-made in its translation, otherwise
// `english` itself - so with English in force, and for words the host passes on
// verbatim (PluginRepo's sha256 and I/O errors), the text is byte for byte what it
// was.
std::string trStoredReason(const std::string& english);

// ===========================================================================
// THE PAGE'S SHARED PARTS
// ===========================================================================

// One picture of a plugin as the page knows it: the catalogue's record, and where
// the fetch has got to (AppWindow::pictureStatus).
enum class PictureState { None, Pending, Ready, Failed };
struct StorePicture {
    std::string sha256;   // the cache key, and the texture's
    std::string caption;  // one line under the frame
    int width = 0;        // advisory (the catalogue's); the frame is 16:10 whatever it says
    int height = 0;
    PictureState state = PictureState::None;
    std::string path;     // Ready: the cached file
    std::string reason;   // Failed: PluginRepo's English words
};

// THE DECODED PICTURES, kept for the window's life. One GL texture per sha256,
// made the first time a Ready picture is drawn - decoded with core::decodePng
// (refusing anything that is not a PNG, over 4096 px a side, or damaged) and
// uploaded once. A picture that will not decode keeps its reason and is not tried
// again. releaseAll() deletes every texture: call it when the window closes. At
// most ONE picture is decoded per frame, so a plugin with eight does not freeze it.
class PagePictureCache {
public:
    struct Entry {
        unsigned tex = 0;     // GL texture name, 0 when it did not decode
        int width = 0;
        int height = 0;
        std::string error;    // why it did not decode (core::decodePng's words)
    };
    PagePictureCache() = default;
    // The textures are owned: a copy would delete them twice.
    PagePictureCache(const PagePictureCache&) = delete;
    PagePictureCache& operator=(const PagePictureCache&) = delete;
    ~PagePictureCache();

    // The decoded picture for `p` (state Ready), decoding it now unless a decode
    // already happened this frame; null while it is not yet available.
    const Entry* find(const StorePicture& p);
    void beginFrame() { decodedThisFrame_ = false; }
    void releaseAll();
    std::size_t size() const { return entries_.size(); }

private:
    std::map<std::string, Entry> entries_;
    bool decodedThisFrame_ = false;
};

// What a page remembers between frames. Owned by the CALLER (a deck), because it
// outlives one frame; one per window, so the store's page and the fitted page
// each keep their own.
struct ModulePageState {
    bool showDetails = false;  // SHOW DETAILS / HIDE DETAILS
    float stripX = 0.0f;       // the pictures' horizontal scroll, in pixels
    float stripTarget = 0.0f;  // where the arrow keys are taking it
    void reset() { *this = ModulePageState{}; }
};

// One row of the DETAILS grid.
struct PageFact {
    std::string key;     // already translated
    std::string value;   // already translated
    bool hatched = false;  // no source: drawn ruled with the reason lettered over it
    bool copyable = false; // a URL or a digest: drawn as selectable text
    ImU32 tone = 0;        // 0 = the default ivory
};

// The facts in order: MAKER, LICENCE, VERSION, PLUGIN ABI, DOWNLOAD, BUILDS FOR,
// REACHES, HOMEPAGE, SHA-256, PUBLISHED, then - when fitted - ON THIS MACHINE and
// FILE, and RETIRED BELOW when set. A fact with no source is hatched and says why
// ("not stated", "not recorded", "not read"); nothing is invented.
std::vector<PageFact> modulePageFacts(const ModulePlate& m);

// The names of the page's sections, in the order they are drawn (these are also
// the census names, "store:section:<name>"): screenshots, whatitdoes, whatsnew,
// then beforeyoufitit ONLY when `hasNoticeBox` (a plugin with a legal notice that
// is not fitted yet), then details.
std::vector<std::string> modulePageSections(bool hasNoticeBox);

// WHAT'S NEW: `whatsNew` when the catalogue has one, otherwise "<version>: first
// release." - a plugin whose catalogue says nothing new is a first release.
std::string moduleWhatsNewText(const ModulePlate& m);

// What the shared body is handed. The pointers are borrowed for the call.
struct ModulePageIn {
    const char* census = "store";             // "store" or "fitted": the census prefix
    std::string id;                           // the catalogue id (census and texture names)
    const ModulePlate* plate = nullptr;
    const std::vector<StorePicture>* pictures = nullptr;  // null/empty: none published
    bool catalogued = true;     // false: not in the catalogue - DETAILS only
    // THE "declared by the maker, not enforced" sentence under the DETAILS grid. The
    // fitted page says it beside its own reach list and so turns this off.
    bool showReachLead = true;
    bool noticeBox = false;     // draw BEFORE YOU FIT IT with the tick
    bool* noticeTick = nullptr; // the tick's state (store only)
    float width = 0.0f;         // the column the page is laid out in
    // The category glyph behind a plugin with no pictures: the catalogue id.
    const char* glyphCategory = "";
};

// Draws the page from SCREENSHOTS down at the ImGui cursor, `in.width` wide, and
// leaves the cursor below it. ALL CALLED FROM INSIDE A CHILD WINDOW the caller
// owns. Draws only what `in` says; it raises no request and changes no state but
// `state` and the tick. The sections draw in the order modulePageSections() gives.
void drawModulePageBody(const ModulePageIn& in, ModulePageState& state,
                        PagePictureCache& pictures);

// ===========================================================================
// THE GLYPHS - one per category, drawn with the draw list in phosphor outline
// ===========================================================================
//
// The shapes are the mock-up's SVG paths (an aeroplane, a ship, a satellite, a
// meter dial, a radio mast, a map pin), flattened to polylines once and drawn at
// any size with a faint glow beneath. `category` is the catalogue id; anything
// else (empty, unknown) draws the dial. `box` is the side of the square the glyph
// is centred in. Draws only.
void drawCategoryGlyph(ImDrawList* dl, const ImVec2& tl, float box, const std::string& category,
                       ImU32 colour, float strokePx);

// ===========================================================================
// THE STORE
// ===========================================================================

// The store's categories in the order the BROWSE tab shows them. VOICE AND DATA
// is the on-screen name of the catalogue category `broadcast`; OTHER holds an
// entry with no category or one this build does not know (today the regional
// Radar Sweep).
enum class StoreCategory {
    Aircraft,
    Marine,
    SatellitesWeather,
    MetersPaging,
    VoiceData,
    MapsTools,
    Other,
};
inline constexpr int kStoreCategoryCount = 7;
StoreCategory storeCategoryFor(const std::string& catalogueId);
const char* storeCategoryHeading(StoreCategory c);      // translated, capitals
const char* storeCategoryCensusName(StoreCategory c);   // "aircraft", ..., "voice", "maps", "other"

// The install state the catalogue card speaks for, from the install records and the
// scan by plugin id and version (PluginRepo::installStateFor).
enum class StoreInstallKind { NotInstalled, Installed, UpdateAvailable, NewerInstalled };

// One catalogue row, as the wiring supplies it.
struct StoreModule {
    // Everything the page draws (see ModulePlate): identity, what it does, facts.
    ModulePlate plate;

    // PluginCatalogEntry::id - the key an update is planned against, the page is
    // opened by, and the census names a card by.
    std::string id;

    // IS THERE A BUILD THIS MACHINE COULD RUN? PluginCatalogEntry::compatible
    // (abiVersion exactly this host's) AND thisPlatform() != nullptr. A STABLE
    // fact about the entry (not "a transfer is in flight").
    bool installableHere = false;

    // WHETHER THIS HOST HAS A BUILD AT ALL, and which operating systems the entry
    // publishes one for: what the build badge (WINDOWS ONLY / LINUX ONLY) says
    // when this host has none.
    bool haveBuildHere = false;
    bool buildsWindows = false;
    bool buildsLinux = false;

    StoreInstallKind install = StoreInstallKind::NotInstalled;
    std::string installedVersion;  // the newest installed version; "" when none

    // WHY GET MAY NOT BE PRESSED, or empty when it may - from the SAME predicate the
    // desktop's key uses (AppWindow::pluginInstallBlockedReason), so the sentence
    // under the key and the key itself can never disagree. Asked with the notice
    // acknowledged only for the plugin whose page is open and ticked.
    std::string blockedReason;
    // THE SAME PREDICATE ASKED AS IF THE MAKER'S NOTICE HAD BEEN ACKNOWLEDGED. Empty
    // means "nothing but the notice stands in the way"; it also lets ADD ALL count
    // the modules one tick would add. Identical to blockedReason for a module with
    // no notice.
    std::string blockedReasonIfAcknowledged;

    // From PluginRepo::planUpdates, when a plan exists for this id. Both empty when
    // none does.
    std::string updateToVersion;
    std::string updateReason;  // PluginUpdate::reason, verbatim
    // UpdateAvailable by version but not plannable here (no build for this host,
    // another ABI): the English reason the UPDATE key is greyed, else empty.
    std::string updateBlockedReason;

    // The pictures the catalogue names, with the fetch's progress.
    std::vector<StorePicture> pictures;
};

// ONE OLD COPY AN UPDATE LEFT BEHIND, as the store shows it: core::supersededPlugins
// decided it may go, and this is what the user is told about it before it does.
struct StoreOldCopy {
    std::string name;         // the plugin, as it declares itself
    std::string version;      // the old copy's
    std::string file;         // the old copy's module file - the one that goes
    std::string keptVersion;  // the copy that stays and is running
};

// Everything the store draws that it cannot work out for itself.
struct PluginStoreModel {
    std::vector<StoreModule> modules;

    // THE OLD COPIES UPDATES LEFT ON DISK, every one of which may be removed
    // (core/plugin_cleanup.hpp). Non-empty draws "CLEAN UP OLD VERSIONS (N)" at the
    // foot of the UPDATES tab, which removes all of them after ONE confirmation.
    std::vector<StoreOldCopy> oldCopies;
    // What the last clean-up did (pluginCleanupReport).
    std::string cleanupReport;

    // AppWindow::pluginCatalogueUrl_ - where the catalogue was read from.
    std::string sourceUrl;

    // This host's platform as the catalogue spells it ("windows/x64"): what the
    // page's header line says the size and build are OF.
    std::string hostPlatform;

    // ARE THERE ROWS? AppWindow::catalog_ being non-empty and nothing more.
    bool haveCatalogue = false;

    // AppWindow::catalogStatus_ / catalogError_, verbatim. A fetch failure is the
    // user's evidence and is never paraphrased. sourceError is the reason a refresh
    // failed (and a kept copy is on screen, or nothing is).
    std::string sourceStatus;
    std::string sourceError;

    // THE KEPT COPY (0.99.72): seconds since the epoch of the read behind the rows
    // (0 = unknown, never drawn as 1970), and whether the rows are the copy kept on
    // disk from an earlier session rather than a read this session.
    std::int64_t catalogueReadTime = 0;
    bool catalogueFromCache = false;
    // sourceError is a REFRESH THAT FAILED (the rows on screen are then not the latest),
    // as against a warning beside a catalogue that was read ("the catalogue loaded, but
    // could not be kept for the next start"): the top bar says "could not refresh" only
    // for the first. The second is drawn as a note above the list.
    bool refreshFailed = false;
    // A picture is being fetched or is queued: the one transfer slot is held, so CHECK
    // AGAIN waits (it would be refused) - but nothing draws a CANCEL for it.
    bool pictureBusy = false;

    // A fetch or a download is in flight, with PluginRepo::progress() and the name
    // and id of what is moving. progress stays at 0 when the server sends no
    // Content-Length, and the key's line then sweeps rather than inventing a figure.
    bool busy = false;
    float progress = 0.0f;
    std::string busyLabel;
    std::string busyId;  // the plugin being fitted or updated, empty for a catalogue read

    // AppWindow::installReport_ / installError_, verbatim, and the id of the plugin
    // they concern (AppWindow::installResultId_): the page of THAT plugin shows them
    // under its header key and no other page does.
    std::string resultReport;
    std::string resultError;
    std::string resultId;

    // --- the GET EVERYTHING run ---------------------------------------------
    // The same queue ADD ALL always was: N transfers through the single install
    // path, one after another.
    bool addAllRunning = false;
    std::string addAllProgress;
    std::string addAllSummary;
    bool addAllFailed = false;
};

// ---------------------------------------------------------------------------
// WHAT A CARD'S KEY SAYS - pure, so it can be checked without a frame
// ---------------------------------------------------------------------------
enum class StoreKeyKind {
    Get,        // GET: not installed
    Fitting,    // FITTING...: this plugin's transfer is running
    Installed,  // INSTALLED: some version is here and the catalogue has nothing newer
    Update,     // UPDATE: here, and the catalogue offers a newer build
};

struct StoreKeyIn {
    std::string busyId;      // PluginStoreModel::busyId
    bool busyAny = false;    // PluginStoreModel::busy: a transfer is running somewhere
    bool onPage = false;     // this is the page key (a card's GET on a notice plugin is not greyed)
    bool noticeTicked = false;  // the page's tick, for THIS plugin
};

struct StoreKey {
    StoreKeyKind kind = StoreKeyKind::Get;
    bool enabled = false;
    // English. Why the key is greyed, when it is; drawn translated on hover.
    std::string reason;
    // GET on a card whose plugin carries a legal notice that has not been ticked:
    // pressing it OPENS THE PAGE, where the notice has to be read.
    bool opensPage = false;
};

// THE ONE DECISION. By install state first (FITTING while this plugin is moving,
// INSTALLED for an installed or newer one, UPDATE for an older one), then by the
// gate: a build for this host, the ABI, a licence, no transfer running, and the
// notice. The reasons are the existing ones, in English.
StoreKey storeKeyFor(const StoreModule& sm, const StoreKeyIn& in);

const char* storeKeyLabel(StoreKeyKind k);  // translated: GET / FITTING... / INSTALLED / UPDATE
// "get", "fitting", "installed", "update", or "greyed" for a key that cannot act:
// the census state of "store:key:<id>:<state>".
const char* storeKeyCensusState(const StoreKey& k);

// The badge words. EXPERIMENTAL is on the entry; a BUILD badge (WINDOWS ONLY or LINUX
// ONLY, translated) appears only when this host has NO build for the entry and the
// entry's builds are all one operating system. Empty otherwise.
const char* storeBuildBadge(const StoreModule& sm);
const char* storeExperimentalBadge();

// The line under a page's name: "<maker> · version <v> · <size> · <builds>", the
// size and builds of THIS host's build, or "no build for this system" in their
// place. `hostPlatform` is "windows/x64" or empty.
std::string storeHeaderMeta(const StoreModule& sm, const std::string& hostPlatform);

// "%s to %s": the update's two versions.
std::string storeFromTo(const std::string& from, const std::string& to);

// The count on the UPDATES tab: modules whose update the planner has a plan for.
int storeUpdateCount(const PluginStoreModel& m);

// Case-insensitive (ASCII) substring over name, summary, description and the
// category's on-screen word. An empty query matches everything.
bool storeMatchesQuery(const StoreModule& sm, const std::string& lowerQuery);

// One category's cards, as indices into PluginStoreModel::modules, sorted by name.
struct StoreSection {
    StoreCategory category = StoreCategory::Other;
    std::vector<int> modules;
};
// The sections that have a card, in the fixed order, for `lowerQuery`.
std::vector<StoreSection> storeBrowseSections(const PluginStoreModel& m,
                                              const std::string& lowerQuery);
// The UPDATES tab's rows: planned updates matching the query, sorted by name.
std::vector<int> storeUpdateRows(const PluginStoreModel& m, const std::string& lowerQuery);

// The top bar's catalogue line and its tone.
enum class StoreLineTone { Muted, Amber };
struct StoreCatalogueLine {
    std::string text;
    StoreLineTone tone = StoreLineTone::Muted;
    bool cached = false;  // the kept copy is what is on screen: noted "store:catalogue:cache"
};
// "Catalogue read HH:MM" (or with the date when it was not today), "Catalogue from
// <date>; could not refresh: <reason>" in amber when a kept copy is on screen after
// a failed refresh, "CATALOGUE NOT READ" before the first read. `nowUnix` is the
// clock and `localTime` converts seconds to the local broken-down time, so a test
// can fix both; the default uses the machine's.
StoreCatalogueLine storeCatalogueLine(const PluginStoreModel& m, std::int64_t nowUnix);

// Columns of cards: three at 1120 px of content and wider, two below. (The window's
// minimum usable width is 900.)
int storeColumnsFor(float contentWidth);
inline constexpr float kStoreMinWidth = 900.0f;

// THE SIZE THIS WINDOW SETS ITS PROSE IN, from the theme's own ladder (the panel
// size): see fonts.hpp for why it exists at all.
float storeProsePx();

// THE FLAGS OF THE PANE THE WHOLE STORE IS DRAWN INTO, owned here so the window
// that draws it and the test that measures it cannot disagree. The view's own
// body child scrolls, so this pane does not.
ImGuiWindowFlags storeFaceWindowFlags();

// The persistent state of the window. Owned by the CALLER because it outlives one
// frame and the caller may persist it; the view edits it in place.
struct PluginStoreDeck {
    // The search text. A fixed buffer because it is handed straight to InputText.
    char search[128] = {0};

    int tab = 0;               // 0 BROWSE, 1 UPDATES
    // The plugin whose page is open ("" = the grid). An id, not an index: a
    // refresh can reorder the catalogue underneath.
    std::string pageId;
    ModulePageState page;

    // The legal-notice acknowledgement, which belongs to the plugin whose page is
    // open. The view clears it whenever the page changes, so a tick given to the
    // plugin the user just read about is never carried to the next one.
    bool legalAck = false;

    // THE GET EVERYTHING ACKNOWLEDGEMENT, a DIFFERENT tick: it covers every plugin
    // in the run that carries a notice, and is not persisted anywhere.
    bool addAllAck = false;
};

// THE CATALOGUE IS BEING REPLACED: every consent given against the old one goes
// with it - the page's tick, and the GET EVERYTHING tick, because both were given
// against notices the OLD catalogue listed. Called by AppWindow::startCatalogFetch
// at the moment the ground moves.
void forgetCatalogueConsent(PluginStoreDeck& deck);

// ===========================================================================
// GET EVERYTHING - the whole decision, in one pure function
// ===========================================================================
//
// WHAT IT PICKS AND WHAT THE KEY SAYS, with no ImGui in it. The window does
// nothing with this but draw it and, on a press, hand the request back.
struct AddAllPlan {
    // Indices into PluginStoreModel::modules, in catalogue order. Every one is a
    // module whose own gate was empty, re-tested at the moment it starts.
    std::vector<int> install;
    std::vector<int> update;

    // "NAME - reason", one per module the run will pass over. NAMED, because "17
    // installed, 7 skipped" tells the user nothing they can act on.
    std::vector<std::string> skipped;

    // How many of `skipped` are held back by a maker's notice alone - the ones the
    // tick would add. Zero once it is ticked.
    int heldByNotice = 0;

    // The engraving the key used to carry ("ADD ALL PLUGINS", or the counts). The
    // store's key now always reads GET EVERYTHING; this stays for the log and the
    // web page.
    std::string label;

    // Empty when the key may be pressed. A dead key ALWAYS says why.
    std::string blockedReason;
};

// `noticesAcknowledged` is PluginStoreDeck::addAllAck: it swaps each module's
// blockedReason for its blockedReasonIfAcknowledged.
AddAllPlan planAddAll(const PluginStoreModel& model, bool noticesAcknowledged);

// ===========================================================================
// OLD VERSIONS - the words, pure, so they can be checked without a frame
// ===========================================================================
//
// The key: "CLEAN UP OLD VERSIONS (3)".
std::string storeCleanupKeyLabel(std::size_t count);
// One line of the confirmation: "ADS-B 1.0.0 - adsb-1.0.0.dll (1.1.0 stays)".
std::string storeOldCopyLine(const StoreOldCopy& c);
// The confirmation's own key: "Remove 1 file" / "Remove 3 files".
std::string storeCleanupConfirmLabel(std::size_t count);
// WHAT A CLEAN-UP DID, in one or more sentences for the panel. Empty when it did
// nothing.
std::string pluginCleanupReport(const cascade::core::PluginCleanupResult& r);

// Draws the CLEAN UP OLD VERSIONS key, its one confirmation popup and the report
// under it, at the ImGui cursor, `width` wide. Returns true on the frame the
// confirmation was ACCEPTED (the caller removes what its own state still calls
// superseded). Shared by the store's UPDATES tab and the fitted modules window's
// foot, so the two cannot disagree. `censusKey` names the key's census rect
// ("storekey:cleanup" in the store - the name existing tests find it by).
bool drawCleanupFoot(const std::vector<StoreOldCopy>& oldCopies, const std::string& report,
                     bool busy, float width, const char* censusKey, const char* censusYes);

// ===========================================================================
// THE SHARED KEY AND CHIP VOCABULARY (used by both windows)
// ===========================================================================
// A key with an outline in `ink` and its word in the legend face: the GET / UPDATE
// / REMOVE / STOP family. `enabled` false draws it greyed. Returns true on a press
// of an enabled key. `out` receives the rectangle drawn (when given). `hoverText`,
// when non-empty, is the tooltip (always shown for a greyed key: it says why).
struct KeyRect {
    ImVec2 tl;
    ImVec2 br;
};
bool drawOutlineKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                    ImU32 ink, bool enabled, const char* id, const char* hoverText,
                    KeyRect* out = nullptr);
// The width an outline key needs for `label`.
float outlineKeyWidth(const char* label);
float outlineKeyHeight();
// The small chassis-grey key of the top bar (CHECK AGAIN, SCAN AGAIN, SHOW DETAILS):
// proud metal with its word engraved.
bool drawChassisKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                    bool enabled, const char* id, KeyRect* out = nullptr);
float chassisKeyWidth(const char* label);
// The search field of a top bar at `tl`, `w` wide, with the loupe and the hint.
// Returns true when the text changed; the field's rectangle is in `out`.
bool drawSearchField(const ImVec2& tl, float w, const char* hint, char* buf, std::size_t bufSize,
                     KeyRect* out);
// A section heading with its rule: small capitals, a line to the right edge.
// Returns the height it took.
float drawSectionHeading(ImDrawList* dl, const ImVec2& tl, float width, const char* text);
// `text` cut with "..." so it fits `maxW`, drawn at `at`. Returns the width drawn.
float addEllipsized(ImDrawList* dl, ImFont* font, float px, const ImVec2& at, ImU32 col,
                    const char* text, float maxW);
// The same cut, as a string, for a test.
std::string ellipsize(ImFont* font, float px, const char* text, float maxW);

// ===========================================================================
// THE VIEW
// ===========================================================================
class PluginStoreView {
public:
    ~PluginStoreView();

    // Draws the whole window's content into the CURRENT ImGui window, filling
    // `width` x `height`. `model` is borrowed for the call only.
    void draw(float width, float height, const PluginStoreModel& model, PluginStoreDeck& deck);

    // Deletes every picture texture. The caller does it when the window closes (the
    // pictures are kept for the window's life, and no longer).
    void releasePictures() { pictures_.releaseAll(); }

    // --- what the last draw() asked for ------------------------------------
    //
    // Requests rather than callbacks: the caller is the only object that knows
    // about the plugin host, the repository and the worker threads, and it applies
    // them AFTER the frame. All are cleared at the start of every draw(), so a
    // request is answered once or not at all.

    // CHECK NOW / CHECK AGAIN was pressed: fetch the catalogue at model.sourceUrl.
    bool checkNowRequested() const { return checkNow_; }
    // CANCEL was pressed during a transfer.
    bool cancelRequested() const { return cancel_; }
    // GET was pressed on this index into model.modules, or -1. The view only
    // offers it where the gate is empty, but the caller must re-test.
    int fitRequested() const { return fitIndex_; }
    // UPDATE was pressed on this index into model.modules, or -1.
    int updateRequested() const { return updateIndex_; }
    // GET EVERYTHING was pressed. The caller re-plans from its own state.
    bool addAllRequested() const { return addAll_; }
    // UPDATE ALL was pressed.
    bool updateAllRequested() const { return updateAll_; }
    // CLEAN UP OLD VERSIONS was pressed AND its one confirmation accepted.
    bool cleanupRequested() const { return cleanup_; }
    // The id of the plugin whose page was OPENED this frame (the edge), or empty:
    // the caller asks for its pictures then. pageId() is the page showing now.
    const std::string& pageOpened() const { return pageOpened_; }
    const std::string& pageId() const { return pageId_; }

private:
    bool cleanup_ = false;
    bool checkNow_ = false;
    bool cancel_ = false;
    bool addAll_ = false;
    bool updateAll_ = false;
    int fitIndex_ = -1;
    int updateIndex_ = -1;
    std::string pageOpened_;
    std::string pageId_;
    std::string lastPage_;
    int lastTab_ = -1;
    PagePictureCache pictures_;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_STORE_VIEW_HPP
