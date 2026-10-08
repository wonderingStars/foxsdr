// Tests for gui/plugin_store_view.hpp - the pure half of the PLUGIN STORE (0.99.72,
// the shop-window redesign) and of the page parts it shares with the FITTED MODULES
// window: what a card's key says for every install state, the badge rules, the
// category sections and their order, the UPDATES count and rows, the search, the
// order of a page's sections, the first-release line, the facts of the DETAILS
// grid, the top bar's catalogue line, and the reach words that survive from the
// data plate.
//
// WHY THESE ARE THE FUNCTIONS THAT MATTER. Each one turns a record into words a
// user will believe, and several are the difference between three things that all
// look like an empty list:
//
//   not declared   a catalogue row whose binary the catalogue did not describe.
//   not known      a file that IS here and that the host refused: it was read and
//                  rejected, which is not the same as asking for nothing.
//   publishes to   a module that was read, declares only inward capabilities, and
//   the host only  genuinely reaches nothing outward.
//
// Collapsing any two of those reports our own ignorance as the maker's silence, or
// a refusal as harmlessness; the checks pin them as DIFFERENT strings rather than
// each being non-empty, because "each is non-empty" would survive the collapse.
//
// There is no drawing in the first half of this file: ImU32 is a plain integer and
// the theme's colours are compile-time constants. The second half loads the real
// typefaces (as test_bench_text_fits does) to check the words fit in every
// language the application is translated into.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "gui/fonts.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/theme.hpp"
#include "test_check.hpp"

using cascade::gui::AddAllPlan;
using cascade::gui::ModulePlate;
using cascade::gui::moduleReachColour;
using cascade::gui::moduleReachSummary;
using cascade::gui::modulePageFacts;
using cascade::gui::modulePageSections;
using cascade::gui::moduleWhatsNewText;
using cascade::gui::planAddAll;
using cascade::gui::PluginStoreModel;
using cascade::gui::StoreCategory;
using cascade::gui::StoreInstallKind;
using cascade::gui::StoreKey;
using cascade::gui::StoreKeyIn;
using cascade::gui::StoreKeyKind;
using cascade::gui::StoreModule;
using cascade::gui::storeBrowseSections;
using cascade::gui::storeBuildBadge;
using cascade::gui::storeCatalogueLine;
using cascade::gui::storeCategoryFor;
using cascade::gui::storeCategoryHeading;
using cascade::gui::storeColumnsFor;
using cascade::gui::storeFromTo;
using cascade::gui::storeHeaderMeta;
using cascade::gui::storeKeyCensusState;
using cascade::gui::storeKeyFor;
using cascade::gui::storeKeyLabel;
using cascade::gui::storeMatchesQuery;
using cascade::gui::storeProsePx;
using cascade::gui::storeUpdateCount;
using cascade::gui::storeUpdateRows;
namespace theme = cascade::gui::theme;

namespace {

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool allDistinct(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return std::unique(v.begin(), v.end()) == v.end();
}

// A CATALOGUE ROW: known to exist, never read.
ModulePlate catalogueRow() {
    ModulePlate m;
    m.name = "ADS-B";
    m.version = "1.2.0";
    m.maker = "FoxSDR";
    m.licence = "PolyForm-Noncommercial-1.0.0";
    return m;
}

// A FITTED, LOADED, STARTED module whose declaration WAS read.
ModulePlate fittedRow(std::uint32_t caps) {
    ModulePlate m = catalogueRow();
    m.fileName = "adsb-1.2.0.dll";
    m.fitted = true;
    m.loaded = true;
    m.running = true;
    m.haveCapabilities = true;
    m.capabilities = caps;
    m.haveTuneGrant = true;
    m.tuneGranted = false;
    return m;
}

// A FILE THE HOST WOULD NOT HAVE: fitted and not loaded, no capability list.
ModulePlate refusedRow() {
    ModulePlate m;
    m.fileName = "broken-9.9.9.dll";
    m.fitted = true;
    m.haveDescriptor = false;
    m.refusalReason = "ABI mismatch: expected 3, plugin reports 2";
    return m;
}

// ---------------------------------------------------------------------------
// 1. moduleReachSummary / moduleReachColour - kept from the data plate
// ---------------------------------------------------------------------------
void testReachSummary() {
    const std::string never = moduleReachSummary(catalogueRow());
    const std::string refused = moduleReachSummary(refusedRow());
    const std::string inward = moduleReachSummary(fittedRow(CASCADE_CAP_DECODER));
    CHECK(allDistinct({never, refused, inward}));
    CHECK(never == "not declared until it is fitted");
    CHECK(refused == "not known: the host did not accept this file");
    CHECK(inward == "publishes to the host only");
    CHECK(!has(refused, "until it is fitted"));

    // NEVER "REACHES NOTHING": every plugin here is native code in this process.
    for (const std::string& s : {never, refused, inward, moduleReachSummary(fittedRow(0u)),
                                 moduleReachSummary(fittedRow(CASCADE_CAP_PANEL)),
                                 moduleReachSummary(fittedRow(CASCADE_CAP_BASEMAP)),
                                 moduleReachSummary(fittedRow(CASCADE_CAP_HOST_CLIENT)),
                                 moduleReachSummary(fittedRow(CASCADE_CAP_ALL_KNOWN))}) {
        CHECK(!s.empty());
        CHECK(!has(s, "nothing"));
    }
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_BASEMAP)) == "may fetch from a server it chose");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_TRACK_INFO)) == "may fetch from a server it chose");

    ModulePlate asks = fittedRow(CASCADE_CAP_HOST_CLIENT);
    CHECK(moduleReachSummary(asks) == "asks to move the receiver");
    asks.tuneGranted = true;
    CHECK(moduleReachSummary(asks) == "granted: may move the receiver");
    // A grant never looked up is not a grant.
    asks.haveTuneGrant = false;
    CHECK(moduleReachSummary(asks) == "asks to move the receiver");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_ALL_KNOWN)) == "asks to move the receiver");
}

void testReachColour() {
    CHECK(moduleReachColour(catalogueRow()) == theme::kInkFaint);
    CHECK(moduleReachColour(refusedRow()) == theme::kInkFaint);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_HOST_CLIENT)) == theme::kGold);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_BASEMAP)) == theme::kGold);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_TRACK_INFO)) == theme::kGold);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_DECODER)) == theme::kInkMuted);
    CHECK(moduleReachColour(fittedRow(0u)) == theme::kInkMuted);
    // NEVER RUST, never phosphor: a declared capability is not a fault, and phosphor means working.
    for (const ModulePlate& m : {catalogueRow(), refusedRow(), fittedRow(0u), fittedRow(CASCADE_CAP_ALL_KNOWN)}) {
        CHECK(moduleReachColour(m) != theme::kAlarm);
        CHECK(moduleReachColour(m) != theme::kAlarmHot);
        CHECK(moduleReachColour(m) != theme::kPhosphor);
    }
}

// ---------------------------------------------------------------------------
// 2. a store row, and the model around it
// ---------------------------------------------------------------------------

StoreModule row(const char* id = "example", const char* name = "Example", const char* category = "aircraft") {
    StoreModule sm;
    sm.id = id;
    sm.plate = catalogueRow();
    sm.plate.name = name;
    sm.plate.category = category;
    sm.plate.summary = "Decodes something on a band.";
    sm.plate.blurb = "A long description of what it does, over several sentences.";
    sm.installableHere = true;
    sm.haveBuildHere = true;
    sm.buildsWindows = true;
    return sm;
}

StoreModule installedRow(const char* id = "example") {
    StoreModule sm = row(id);
    sm.install = StoreInstallKind::Installed;
    sm.installedVersion = "1.2.0";
    sm.plate.fitted = true;
    sm.plate.loaded = true;
    sm.plate.running = true;
    sm.plate.haveCapabilities = true;
    sm.plate.capabilities = CASCADE_CAP_DECODER;
    return sm;
}

StoreModule updateRow(const char* id = "example") {
    StoreModule sm = installedRow(id);
    sm.install = StoreInstallKind::UpdateAvailable;
    sm.installedVersion = "1.8.0";
    sm.plate.version = "1.8.1";
    sm.updateToVersion = "1.8.1";
    return sm;
}

// ---------------------------------------------------------------------------
// 3. the card's key, for every state
// ---------------------------------------------------------------------------
void testKeyPerInstallState() {
    const StoreKeyIn none;
    {
        const StoreKey k = storeKeyFor(row(), none);
        CHECK(k.kind == StoreKeyKind::Get);
        CHECK(k.enabled);
        CHECK(!k.opensPage);
        CHECK(k.reason.empty());
        CHECK(std::string(storeKeyLabel(k.kind)) == "GET");
        CHECK(std::string(storeKeyCensusState(k)) == "get");
    }
    {
        // INSTALLED: some version is here and the catalogue has nothing newer - and a NEWER
        // one installed is the same word, never an UPDATE downgrade.
        StoreModule a = installedRow();
        StoreModule b = installedRow();
        b.install = StoreInstallKind::NewerInstalled;
        for (const StoreModule& sm : {a, b}) {
            const StoreKey k = storeKeyFor(sm, none);
            CHECK(k.kind == StoreKeyKind::Installed);
            CHECK(!k.enabled);
            CHECK(std::string(storeKeyLabel(k.kind)) == "INSTALLED");
            CHECK(std::string(storeKeyCensusState(k)) == "installed");
        }
    }
    {
        const StoreKey k = storeKeyFor(updateRow(), none);
        CHECK(k.kind == StoreKeyKind::Update);
        CHECK(k.enabled);
        CHECK(std::string(storeKeyLabel(k.kind)) == "UPDATE");
        CHECK(std::string(storeKeyCensusState(k)) == "update");
        // A plan exists, but a transfer is running somewhere: greyed, and it says why.
        StoreKeyIn busy;
        busy.busyAny = true;
        const StoreKey kb = storeKeyFor(updateRow(), busy);
        CHECK(kb.kind == StoreKeyKind::Update);
        CHECK(!kb.enabled);
        CHECK(kb.reason == "a transfer is already in progress");
        CHECK(std::string(storeKeyCensusState(kb)) == "greyed");
    }
    {
        // NEWER BY VERSION AND NOT PLANNABLE HERE (no build, another ABI): UPDATE, greyed, and
        // the reason is the one the model gave.
        StoreModule sm = updateRow();
        sm.updateToVersion.clear();
        sm.updateBlockedReason = "no build for linux/x64";
        const StoreKey k = storeKeyFor(sm, none);
        CHECK(k.kind == StoreKeyKind::Update);
        CHECK(!k.enabled);
        CHECK(k.reason == "no build for linux/x64");
    }
    {
        // FITTING...: this plugin's own transfer, and no other plugin's.
        StoreKeyIn mine;
        mine.busyId = "example";
        mine.busyAny = true;
        const StoreKey k = storeKeyFor(row("example"), mine);
        CHECK(k.kind == StoreKeyKind::Fitting);
        CHECK(!k.enabled);
        CHECK(std::string(storeKeyLabel(k.kind)) == "FITTING...");
        CHECK(std::string(storeKeyCensusState(k)) == "fitting");
        // ...an update in flight reads FITTING... too, not UPDATE.
        CHECK(storeKeyFor(updateRow("example"), mine).kind == StoreKeyKind::Fitting);
        // Somebody else's transfer: this one is GET, greyed by the gate's own reason.
        StoreKeyIn theirs;
        theirs.busyId = "other";
        theirs.busyAny = true;
        StoreModule sm = row("example");
        sm.blockedReason = "a transfer is already in progress";
        sm.blockedReasonIfAcknowledged = "a transfer is already in progress";
        const StoreKey kt = storeKeyFor(sm, theirs);
        CHECK(kt.kind == StoreKeyKind::Get);
        CHECK(!kt.enabled);
        CHECK(kt.reason == "a transfer is already in progress");
    }
    {
        // GREYED REASONS - each of today's, in English, carried through verbatim.
        for (const char* why : {"no build for windows/x64", "the catalogue entry declares no licence",
                                "already installed",
                                "not compatible with this version (built for plugin ABI 2, this build "
                                "requires exactly 3)"}) {
            StoreModule sm = row();
            sm.blockedReason = why;
            sm.blockedReasonIfAcknowledged = why;
            const StoreKey k = storeKeyFor(sm, none);
            CHECK(k.kind == StoreKeyKind::Get);
            CHECK(!k.enabled);
            CHECK(k.reason == why);
            CHECK(std::string(storeKeyCensusState(k)) == "greyed");
        }
    }
    // Four kinds, four words.
    CHECK(allDistinct({storeKeyLabel(StoreKeyKind::Get), storeKeyLabel(StoreKeyKind::Fitting),
                       storeKeyLabel(StoreKeyKind::Installed), storeKeyLabel(StoreKeyKind::Update)}));
}

void testNoticeGate() {
    // A PLUGIN WITH A LEGAL NOTICE. On a CARD the GET key is enabled and opens the page - the
    // notice cannot be skipped and the key is not greyed for it.
    StoreModule sm = row();
    sm.plate.legalNotice = "Interception may be an offence where you are.";
    sm.blockedReason = "the legal notice must be acknowledged first";
    sm.blockedReasonIfAcknowledged.clear();
    StoreKeyIn card;
    const StoreKey kc = storeKeyFor(sm, card);
    CHECK(kc.kind == StoreKeyKind::Get);
    CHECK(kc.enabled);
    CHECK(kc.opensPage);

    // ON THE PAGE the key is greyed until the tick, with the notice's own reason...
    StoreKeyIn page;
    page.onPage = true;
    const StoreKey kp = storeKeyFor(sm, page);
    CHECK(!kp.enabled);
    CHECK(!kp.opensPage);
    CHECK(kp.reason == "the legal notice must be acknowledged first");
    CHECK(std::string(storeKeyCensusState(kp)) == "greyed");
    // ...and enabled after it.
    page.noticeTicked = true;
    const StoreKey kt = storeKeyFor(sm, page);
    CHECK(kt.kind == StoreKeyKind::Get);
    CHECK(kt.enabled);
    CHECK(kt.reason.empty());
    CHECK(std::string(storeKeyCensusState(kt)) == "get");

    // A tick does not cure what is not the notice: no build for this host stays greyed.
    sm.blockedReason = "no build for linux/x64";
    sm.blockedReasonIfAcknowledged = "no build for linux/x64";
    const StoreKey kn = storeKeyFor(sm, page);
    CHECK(!kn.enabled);
    CHECK(kn.reason == "no build for linux/x64");
    // ...and on a card such a plugin does not open its page either: there is nothing to tick.
    const StoreKey knc = storeKeyFor(sm, card);
    CHECK(!knc.enabled);
    CHECK(!knc.opensPage);
}

// ---------------------------------------------------------------------------
// 4. the badges
// ---------------------------------------------------------------------------
void testBadges() {
    // EXPERIMENTAL is its own word, never part of the name.
    CHECK(std::string(cascade::gui::storeExperimentalBadge()) == "EXPERIMENTAL");

    // THE BUILD BADGE says WHY a key is greyed, so it appears only when THIS host has no build,
    // and says which operating system the entry IS for.
    StoreModule hasBuild = row();
    hasBuild.haveBuildHere = true;
    hasBuild.buildsWindows = true;
    CHECK(std::string(storeBuildBadge(hasBuild)).empty());

    StoreModule winOnly = row();
    winOnly.haveBuildHere = false;
    winOnly.buildsWindows = true;
    winOnly.buildsLinux = false;
    CHECK(std::string(storeBuildBadge(winOnly)) == "WINDOWS ONLY");

    StoreModule linuxOnly = row();
    linuxOnly.haveBuildHere = false;
    linuxOnly.buildsWindows = false;
    linuxOnly.buildsLinux = true;
    CHECK(std::string(storeBuildBadge(linuxOnly)) == "LINUX ONLY");

    // Builds for both and none for this host (another architecture), or for neither named
    // system: no badge - it would say something false.
    StoreModule both = row();
    both.haveBuildHere = false;
    both.buildsWindows = true;
    both.buildsLinux = true;
    CHECK(std::string(storeBuildBadge(both)).empty());
    StoreModule neither = row();
    neither.haveBuildHere = false;
    neither.buildsWindows = false;
    neither.buildsLinux = false;
    CHECK(std::string(storeBuildBadge(neither)).empty());
}

// ---------------------------------------------------------------------------
// 5. the categories
// ---------------------------------------------------------------------------
void testCategories() {
    CHECK(storeCategoryFor("aircraft") == StoreCategory::Aircraft);
    CHECK(storeCategoryFor("marine") == StoreCategory::Marine);
    CHECK(storeCategoryFor("satellites-weather") == StoreCategory::SatellitesWeather);
    CHECK(storeCategoryFor("meters-paging") == StoreCategory::MetersPaging);
    // VOICE AND DATA is the on-screen name of the catalogue's `broadcast`.
    CHECK(storeCategoryFor("broadcast") == StoreCategory::VoiceData);
    CHECK(storeCategoryFor("maps-tools") == StoreCategory::MapsTools);
    // No category, or one this build does not know (a newer catalogue's): OTHER.
    CHECK(storeCategoryFor("") == StoreCategory::Other);
    CHECK(storeCategoryFor("regional-radar") == StoreCategory::Other);
    CHECK(storeCategoryFor("Aircraft") == StoreCategory::Other);  // ids are exact

    const char* want[] = {"AIRCRAFT",        "MARINE",         "SATELLITES AND WEATHER", "METERS AND PAGING",
                          "VOICE AND DATA",  "MAPS AND TOOLS", "OTHER"};
    std::vector<std::string> seen;
    for (int c = 0; c < cascade::gui::kStoreCategoryCount; ++c) {
        seen.push_back(storeCategoryHeading(static_cast<StoreCategory>(c)));
        CHECK(seen.back() == want[c]);
    }
    CHECK(allDistinct(seen));
    CHECK(std::string(cascade::gui::storeCategoryCensusName(StoreCategory::VoiceData)) == "voice");

    // THE SECTIONS come in the fixed order whatever order the catalogue lists them in, each
    // sorted by name, and an empty one is not drawn.
    PluginStoreModel m;
    m.haveCatalogue = true;
    m.modules.push_back(row("m1", "Maps Workbench", "maps-tools"));
    m.modules.push_back(row("a2", "Beta Air", "aircraft"));
    m.modules.push_back(row("v1", "DMR", "broadcast"));
    m.modules.push_back(row("a1", "alpha air", "aircraft"));
    m.modules.push_back(row("o1", "Radar Sweep", ""));
    m.modules.push_back(row("s1", "Satellites", "satellites-weather"));
    const std::vector<cascade::gui::StoreSection> sec = storeBrowseSections(m, "");
    CHECK(sec.size() == 5u);
    if (sec.size() == 5u) {
        CHECK(sec[0].category == StoreCategory::Aircraft);
        CHECK(sec[1].category == StoreCategory::SatellitesWeather);
        CHECK(sec[2].category == StoreCategory::VoiceData);
        CHECK(sec[3].category == StoreCategory::MapsTools);
        CHECK(sec[4].category == StoreCategory::Other);
        // By name, case-insensitively: "alpha air" before "Beta Air".
        CHECK(sec[0].modules.size() == 2u);
        if (sec[0].modules.size() == 2u) {
            CHECK(m.modules[static_cast<std::size_t>(sec[0].modules[0])].id == "a1");
            CHECK(m.modules[static_cast<std::size_t>(sec[0].modules[1])].id == "a2");
        }
        CHECK(m.modules[static_cast<std::size_t>(sec[4].modules[0])].id == "o1");
    }
    // Every module is in exactly one section.
    std::size_t total = 0;
    for (const cascade::gui::StoreSection& s : sec) { total += s.modules.size(); }
    CHECK(total == m.modules.size());
}

// ---------------------------------------------------------------------------
// 6. the search
// ---------------------------------------------------------------------------
void testSearch() {
    StoreModule sm = row("adsb", "ADS-B Aircraft Decoder", "aircraft");
    sm.plate.summary = "Plots aircraft at 1090 MHz.";
    sm.plate.blurb = "Decodes Mode S squitters into positions; flight trails and callsigns.";
    CHECK(storeMatchesQuery(sm, ""));
    CHECK(storeMatchesQuery(sm, "ads-b"));          // name
    CHECK(storeMatchesQuery(sm, "1090"));           // summary
    CHECK(storeMatchesQuery(sm, "squitters"));      // description
    CHECK(storeMatchesQuery(sm, "aircraft"));       // category word and name
    CHECK(!storeMatchesQuery(sm, "navtex"));
    // The query arrives lower-cased by the caller; the haystack is folded here.
    CHECK(storeMatchesQuery(sm, "mode s"));
    // THE CATEGORY'S ON-SCREEN WORD: a broadcast plugin is found by "voice".
    StoreModule dmr = row("dmr", "DMR", "broadcast");
    dmr.plate.summary = "Digital radio.";
    dmr.plate.blurb = "Digital radio.";
    CHECK(storeMatchesQuery(dmr, "voice and data"));
    CHECK(!storeMatchesQuery(dmr, "marine"));

    // The sections narrow with the query, and a section with no match vanishes.
    PluginStoreModel m;
    m.haveCatalogue = true;
    m.modules = {sm, dmr};
    CHECK(storeBrowseSections(m, "dmr").size() == 1u);
    CHECK(storeBrowseSections(m, "nothing-matches-this").empty());
    CHECK(storeBrowseSections(m, "").size() == 2u);
}

// ---------------------------------------------------------------------------
// 7. the UPDATES tab: the count and the rows
// ---------------------------------------------------------------------------
void testUpdatesTab() {
    PluginStoreModel m;
    m.haveCatalogue = true;
    m.modules = {row("a", "Alpha"), updateRow("b"), installedRow("c"), updateRow("d")};
    m.modules[1].plate.name = "Zulu Update";
    m.modules[3].plate.name = "Bravo Update";
    // The count is the planner's plans: an UpdateAvailable the planner has no plan for (no build
    // here) is not counted, so the tab and the rail's "n UPD" cannot disagree.
    CHECK(storeUpdateCount(m) == 2);
    m.modules.push_back(updateRow("e"));
    m.modules.back().updateToVersion.clear();
    m.modules.back().updateBlockedReason = "no build for linux/x64";
    CHECK(storeUpdateCount(m) == 2);
    const std::vector<int> rows = storeUpdateRows(m, "");
    CHECK(rows.size() == 2u);
    if (rows.size() == 2u) {
        // By name: Bravo before Zulu.
        CHECK(m.modules[static_cast<std::size_t>(rows[0])].id == "d");
        CHECK(m.modules[static_cast<std::size_t>(rows[1])].id == "b");
    }
    CHECK(storeUpdateRows(m, "zulu").size() == 1u);
    CHECK(storeUpdateRows(m, "alpha").empty());  // alpha is not an update

    CHECK(storeFromTo("1.8.0", "1.8.1") == "1.8.0 to 1.8.1");
    CHECK(storeFromTo("0.1.1", "0.1.2") == "0.1.1 to 0.1.2");

    PluginStoreModel none;
    none.haveCatalogue = true;
    none.modules = {row("a")};
    CHECK(storeUpdateCount(none) == 0);
    CHECK(storeUpdateRows(none, "").empty());
}

// ---------------------------------------------------------------------------
// 8. the page: the order of its sections, what is new, the header line
// ---------------------------------------------------------------------------
void testPageSections() {
    const std::vector<std::string> plain = modulePageSections(false);
    const std::vector<std::string> noticed = modulePageSections(true);
    CHECK((plain == std::vector<std::string>{"screenshots", "whatitdoes", "whatsnew", "details"}));
    CHECK((noticed == std::vector<std::string>{"screenshots", "whatitdoes", "whatsnew", "beforeyoufitit",
                                               "details"}));
}

void testWhatsNew() {
    ModulePlate p = catalogueRow();
    p.version = "0.1.0";
    // ABSENT: the first-release line, with the version in it.
    CHECK(moduleWhatsNewText(p) == "0.1.0: first release.");
    p.whatsNew = "1.8.1: Keeps phantom aircraft off the map.";
    CHECK(moduleWhatsNewText(p) == "1.8.1: Keeps phantom aircraft off the map.");
    // A version never stated is not invented.
    ModulePlate q = catalogueRow();
    q.version.clear();
    CHECK(moduleWhatsNewText(q) == "?: first release.");
}

void testHeaderMeta() {
    StoreModule sm = row();
    sm.plate.maker = "FoxSDR project";
    sm.plate.version = "1.8.1";
    sm.plate.haveSizeBytes = true;
    sm.plate.sizeBytes = 212000;
    sm.haveBuildHere = true;
    // "<maker> . version <v> . <size> . <builds>", the size and builds of THIS host's build.
    CHECK(storeHeaderMeta(sm, "windows/x64") ==
          "FoxSDR project  \xc2\xb7  version 1.8.1  \xc2\xb7  212 kB  \xc2\xb7  windows/x64");
    // No size stated: no size is invented.
    sm.plate.haveSizeBytes = false;
    CHECK(storeHeaderMeta(sm, "windows/x64") ==
          "FoxSDR project  \xc2\xb7  version 1.8.1  \xc2\xb7  windows/x64");
    // No build for this host: "no build for this system" in their place.
    sm.haveBuildHere = false;
    sm.plate.haveSizeBytes = true;
    CHECK(storeHeaderMeta(sm, "linux/x64") ==
          "FoxSDR project  \xc2\xb7  version 1.8.1  \xc2\xb7  no build for this system");
    // A maker never stated is said so.
    sm.plate.maker.clear();
    CHECK(has(storeHeaderMeta(sm, ""), "maker not stated"));
}

void testPageFacts() {
    ModulePlate p = catalogueRow();
    p.maker = "FoxSDR project";
    p.licence = "MIT";
    p.version = "1.8.1";
    p.haveAbi = true;
    p.abiVersion = 3;
    p.hostAbiVersion = 3;
    p.haveSizeBytes = true;
    p.sizeBytes = 212000;
    p.platforms = "windows/x64";
    p.haveCapabilities = true;
    p.capabilities = CASCADE_CAP_IQ_DECODER | CASCADE_CAP_TRACK_SOURCE;
    p.homepage = "github.com/wonderingStars/foxsdr-plugins";
    p.sha256 = "5b32c437" + std::string(56, 'a');
    p.published = "2026-10-07";
    std::vector<cascade::gui::PageFact> f = modulePageFacts(p);
    // THE ORDER, and what is left out while it is not fitted: no ON THIS MACHINE, no FILE.
    std::vector<std::string> keys;
    for (const auto& x : f) { keys.push_back(x.key); }
    CHECK((keys == std::vector<std::string>{"MAKER", "LICENCE", "VERSION", "PLUGIN ABI", "DOWNLOAD",
                                            "BUILDS FOR", "REACHES", "HOMEPAGE", "SHA-256", "PUBLISHED"}));
    if (f.size() == 10u) {
        CHECK(f[0].value == "FoxSDR project");
        CHECK(f[3].value == "3, matches this build");
        CHECK(f[4].value == "212 kB");
        // REACHES, from the catalogue's capabilities, in today's reach words.
        CHECK(f[6].value == "I/Q decoder, Map targets");
        CHECK(!f[6].hatched);
        // A URL and a digest are for copying.
        CHECK(f[7].copyable);
        CHECK(f[8].copyable);
        CHECK(f[9].value == "2026-10-07");
    }
    // FITTED: ON THIS MACHINE and FILE follow, and a retirement floor when there is one.
    p.fitted = true;
    p.loaded = true;
    p.running = true;
    p.fileName = "adsb-1.8.0.dll";
    p.retirementFloor = "1.7.0";
    f = modulePageFacts(p);
    keys.clear();
    for (const auto& x : f) { keys.push_back(x.key); }
    CHECK((keys == std::vector<std::string>{"MAKER", "LICENCE", "VERSION", "PLUGIN ABI", "DOWNLOAD",
                                            "BUILDS FOR", "REACHES", "HOMEPAGE", "SHA-256", "PUBLISHED",
                                            "ON THIS MACHINE", "FILE", "RETIRED BELOW"}));

    // NOTHING IS INVENTED: every fact with no source is hatched and says so.
    ModulePlate bare = catalogueRow();
    bare.maker.clear();
    bare.licence.clear();
    bare.version.clear();
    const std::vector<cascade::gui::PageFact> g = modulePageFacts(bare);
    for (const auto& x : g) {
        if (x.key == "MAKER" || x.key == "LICENCE" || x.key == "VERSION" || x.key == "PLUGIN ABI" ||
            x.key == "DOWNLOAD" || x.key == "BUILDS FOR" || x.key == "HOMEPAGE" || x.key == "SHA-256" ||
            x.key == "PUBLISHED" || x.key == "REACHES") {
            CHECK(x.hatched);
            CHECK(!x.value.empty());
        }
    }
    // A file the host never read says "not read" - not "not stated", which would blame the maker.
    ModulePlate unread = refusedRow();
    const std::vector<cascade::gui::PageFact> u = modulePageFacts(unread);
    CHECK(u[0].key == "MAKER" && u[0].value == "not read");
    CHECK(u[1].key == "LICENCE" && u[1].value == "not read");
    // ...and a licence that was looked for and is not there is a different sentence.
    CHECK(g[1].value == "none declared");
    // The ABI of a record that did not say is "not recorded", never a mismatch.
    CHECK(g[3].value == "not recorded");
    // A mismatch is lettered gold.
    ModulePlate wrong = catalogueRow();
    wrong.haveAbi = true;
    wrong.abiVersion = 2;
    wrong.hostAbiVersion = 3;
    const std::vector<cascade::gui::PageFact> w = modulePageFacts(wrong);
    CHECK(w[3].value == "2, this build needs 3");
    CHECK(w[3].tone == theme::kGold);
    // REACHES for a catalogue row whose binary was not described: not declared, hatched.
    CHECK(g[6].key == "REACHES" && g[6].hatched);
    CHECK(g[6].value == "not declared until it is fitted");
}

// ---------------------------------------------------------------------------
// 9. the top bar's catalogue line
// ---------------------------------------------------------------------------
void testCatalogueLine() {
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    {
        PluginStoreModel m;
        const cascade::gui::StoreCatalogueLine l = storeCatalogueLine(m, now);
        CHECK(l.text == "CATALOGUE NOT READ");
        CHECK(l.tone == cascade::gui::StoreLineTone::Muted);
        CHECK(!l.cached);
    }
    {
        // READ TODAY: "Catalogue read HH:MM".
        PluginStoreModel m;
        m.haveCatalogue = true;
        m.catalogueReadTime = now;
        const cascade::gui::StoreCatalogueLine l = storeCatalogueLine(m, now);
        CHECK(l.text.size() == std::string("Catalogue read 00:00").size());
        CHECK(l.text.rfind("Catalogue read ", 0) == 0);
        CHECK(l.text[l.text.size() - 3] == ':');
        CHECK(l.tone == cascade::gui::StoreLineTone::Muted);
        CHECK(!l.cached);
        // ...three days ago it carries the date as well.
        m.catalogueReadTime = now - 3 * 86400;
        const cascade::gui::StoreCatalogueLine old = storeCatalogueLine(m, now);
        CHECK(old.text.rfind("Catalogue read 20", 0) == 0);
        CHECK(old.text.size() > l.text.size());
    }
    {
        // A KEPT COPY after a refresh that failed: amber, the date and the reason VERBATIM.
        PluginStoreModel m;
        m.haveCatalogue = true;
        m.catalogueFromCache = true;
        m.catalogueReadTime = now - 86400;
        m.refreshFailed = true;
        m.sourceError = "TLS handshake failed";
        const cascade::gui::StoreCatalogueLine l = storeCatalogueLine(m, now);
        CHECK(l.tone == cascade::gui::StoreLineTone::Amber);
        CHECK(l.cached);
        CHECK(l.text.rfind("Catalogue from 20", 0) == 0);
        CHECK(has(l.text, "; could not refresh: TLS handshake failed"));
    }
    {
        // A CATALOGUE THAT LOADED WITH A WARNING is not a failed refresh: the line says it was read.
        PluginStoreModel m;
        m.haveCatalogue = true;
        m.catalogueReadTime = now;
        m.sourceError = "the catalogue loaded, but could not be kept for the next start";
        const cascade::gui::StoreCatalogueLine l = storeCatalogueLine(m, now);
        CHECK(l.text.rfind("Catalogue read ", 0) == 0);
        CHECK(l.tone == cascade::gui::StoreLineTone::Muted);
    }
    {
        // THE KEPT COPY, nothing asked yet: read, and cached, with no failure to report.
        PluginStoreModel m;
        m.haveCatalogue = true;
        m.catalogueFromCache = true;
        m.catalogueReadTime = now - 86400;
        const cascade::gui::StoreCatalogueLine l = storeCatalogueLine(m, now);
        CHECK(l.cached);
        CHECK(l.tone == cascade::gui::StoreLineTone::Muted);
        // An unrecorded time is never drawn as 1970.
        m.catalogueReadTime = 0;
        CHECK(!has(storeCatalogueLine(m, now).text, "1970"));
        CHECK(has(storeCatalogueLine(m, now).text, "unknown"));
    }
    {
        // Nothing on screen and a failure: still "not read" up here; the reason is in the list.
        PluginStoreModel m;
        m.refreshFailed = true;
        m.sourceError = "could not resolve host";
        CHECK(storeCatalogueLine(m, now).text == "CATALOGUE NOT READ");
    }
}

void testColumns() {
    // Three cards a row at 1234 px and wider, two narrower; 900 is the least usable width.
    CHECK(storeColumnsFor(1480.0f) == 3);
    CHECK(storeColumnsFor(1218.0f) == 3);
    CHECK(storeColumnsFor(1120.0f) == 3);
    CHECK(storeColumnsFor(1119.0f) == 2);
    CHECK(storeColumnsFor(900.0f) == 2);
    CHECK(cascade::gui::kStoreMinWidth == 900.0f);
}

void testProseSize() {
    CHECK(storeProsePx() > cascade::gui::fonts::kTinySize);
    CHECK(storeProsePx() > cascade::gui::fonts::kUiSize);
    CHECK(storeProsePx() == cascade::gui::fonts::kPanelSize);
}

// ---------------------------------------------------------------------------
// 10. GET EVERYTHING - what the key picks
// ---------------------------------------------------------------------------

PluginStoreModel readCatalogue(std::vector<StoreModule> mods) {
    PluginStoreModel m;
    m.modules = std::move(mods);
    m.haveCatalogue = !m.modules.empty();
    m.sourceStatus = "N plugins in the catalogue";
    return m;
}

bool hasNaming(const std::vector<std::string>& v, const char* name) {
    for (const std::string& s : v) {
        if (s.find(name) != std::string::npos) { return true; }
    }
    return false;
}

void testAddAllPlan() {
    {
        std::vector<StoreModule> mods(3, row());
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 3u);
        CHECK(p.update.empty());
        CHECK(p.skipped.empty());
        CHECK(p.blockedReason.empty());
        CHECK(p.label == "ADD ALL PLUGINS");
        CHECK(p.install[0] == 0 && p.install[1] == 1 && p.install[2] == 2);
    }
    {
        // SOME INSTALLED AND CURRENT: not "skipped" - having it is the outcome the key was pressed for.
        std::vector<StoreModule> mods = {row(), installedRow(), row()};
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 2u);
        CHECK(p.install[0] == 0 && p.install[1] == 2);
        CHECK(p.skipped.empty());
        CHECK(p.blockedReason.empty());
    }
    {
        std::vector<StoreModule> mods = {row(), row(), row(), updateRow("u1"), updateRow("u2")};
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 3u);
        CHECK(p.update.size() == 2u);
        CHECK(p.update[0] == 3 && p.update[1] == 4);
        CHECK(p.label == "ADD 3 PLUGINS, UPDATE 2");
    }
    {
        // UPDATE ALL's own question: with nothing but updates in the catalogue.
        std::vector<StoreModule> mods = {updateRow("u1"), updateRow("u2")};
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.empty());
        CHECK(p.update.size() == 2u);
        CHECK(p.label == "UPDATE 2 PLUGINS");
        CHECK(p.blockedReason.empty());
    }
    {
        std::vector<StoreModule> mods(2, installedRow());
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.empty());
        CHECK(!p.blockedReason.empty());
        CHECK(has(p.blockedReason, "already fitted"));
    }
    {
        // AN ENTRY THIS BUILD CANNOT TAKE is skipped AND NAMED, the reason carried through.
        std::vector<StoreModule> mods = {row(), row()};
        mods[1].plate.name = "Inmarsat-C";
        mods[1].installableHere = false;
        mods[1].blockedReason =
            "not compatible with this version (built for plugin ABI 2, this build requires exactly 3)";
        mods[1].blockedReasonIfAcknowledged = mods[1].blockedReason;
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 1u);
        CHECK(p.skipped.size() == 1u);
        CHECK(hasNaming(p.skipped, "Inmarsat-C"));
        CHECK(hasNaming(p.skipped, "plugin ABI 2"));
        CHECK(p.label == "ADD 1 PLUGIN");
        CHECK(p.heldByNotice == 0);
    }
    {
        // A MAKER'S LEGAL NOTICE is the one skip the tick undoes, so it is counted apart.
        std::vector<StoreModule> mods = {row(), row()};
        mods[1].plate.name = "406 MHz Distress Beacon Decoder";
        mods[1].plate.legalNotice = "Interception may be an offence where you are.";
        mods[1].blockedReason = "the legal notice must be acknowledged first";
        mods[1].blockedReasonIfAcknowledged.clear();
        const AddAllPlan held = planAddAll(readCatalogue(mods), false);
        CHECK(held.install.size() == 1u);
        CHECK(held.heldByNotice == 1);
        CHECK(hasNaming(held.skipped, "406 MHz"));
        const AddAllPlan acked = planAddAll(readCatalogue(mods), true);
        CHECK(acked.install.size() == 2u);
        CHECK(acked.skipped.empty());
        CHECK(acked.label == "ADD ALL PLUGINS");
    }
    {
        // NOBODY HAS ASKED: not "everything is up to date".
        PluginStoreModel m;
        const AddAllPlan p = planAddAll(m, false);
        CHECK(has(p.blockedReason, "CHECK NOW"));
        CHECK(!has(p.blockedReason, "already fitted"));
    }
    {
        // ASKED, AND THE ATTEMPT FAILED: no "press CHECK NOW", and no pointer to a deck that is gone.
        PluginStoreModel m;
        m.sourceError = "TLS handshake failed";
        const AddAllPlan p = planAddAll(m, false);
        CHECK(!has(p.blockedReason, "CHECK NOW"));
        CHECK(has(p.blockedReason, "did not return a catalogue"));
        CHECK(!has(p.blockedReason, "CATALOGUE SOURCE"));
    }
    {
        PluginStoreModel m;
        m.sourceStatus = "0 plugins in the catalogue";
        CHECK(has(planAddAll(m, false).blockedReason, "lists no modules"));
    }
    {
        PluginStoreModel m = readCatalogue(std::vector<StoreModule>(3, row()));
        m.busy = true;
        const AddAllPlan p = planAddAll(m, false);
        CHECK(has(p.blockedReason, "transfer is already in progress"));
        CHECK(p.install.size() == 3u);
    }
}

// A CATALOGUE REPLACED TAKES EVERY CONSENT WITH IT: the page's tick and the GET EVERYTHING tick
// were given against the notices the OLD catalogue listed.
void testCatalogueConsentForgotten() {
    std::vector<StoreModule> first = {row(), row()};
    first[1].plate.name = "406 MHz Distress Beacon Decoder";
    first[1].plate.legalNotice = "Interception may be an offence where you are.";
    first[1].blockedReason = "the legal notice must be acknowledged first";
    first[1].blockedReasonIfAcknowledged.clear();
    cascade::gui::PluginStoreDeck deck;
    deck.pageId = "example";
    deck.legalAck = true;
    deck.addAllAck = true;
    CHECK(planAddAll(readCatalogue(first), deck.addAllAck).label == "ADD ALL PLUGINS");

    cascade::gui::forgetCatalogueConsent(deck);
    CHECK(!deck.legalAck);
    CHECK(!deck.addAllAck);
    // The page itself is not closed by a refresh.
    CHECK(deck.pageId == "example");

    std::vector<StoreModule> second = first;
    second.push_back(row());
    second[2].plate.name = "Pager Decoder";
    second[2].plate.legalNotice = "Reading pager traffic may be unlawful where you are.";
    second[2].blockedReason = "the legal notice must be acknowledged first";
    second[2].blockedReasonIfAcknowledged.clear();
    const AddAllPlan p = planAddAll(readCatalogue(second), deck.addAllAck);
    CHECK(p.heldByNotice == 2);
    CHECK(hasNaming(p.skipped, "Pager Decoder"));
    CHECK(p.label == "ADD 1 PLUGIN");
}

// ---------------------------------------------------------------------------
// 11. words kept in English
// ---------------------------------------------------------------------------
void testCleanupWords() {
    CHECK(cascade::gui::storeCleanupKeyLabel(3) == "CLEAN UP OLD VERSIONS (3)");
    cascade::gui::StoreOldCopy c{"ADS-B", "1.0.0", "adsb-1.0.0.dll", "1.1.0"};
    CHECK(cascade::gui::storeOldCopyLine(c) == "ADS-B 1.0.0 - adsb-1.0.0.dll (1.1.0 stays)");
    CHECK(cascade::gui::storeCleanupConfirmLabel(1) == "Remove 1 file");
    CHECK(cascade::gui::storeCleanupConfirmLabel(3) == "Remove 3 files");
    cascade::core::PluginCleanupResult r;
    CHECK(cascade::gui::pluginCleanupReport(r).empty());
    r.removed = {"adsb-1.0.0.dll"};
    CHECK(cascade::gui::pluginCleanupReport(r) == "Removed the old version adsb-1.0.0.dll.");
}

}  // namespace

// ---------------------------------------------------------------------------
// 34-language review: the new words in every catalogue, in the real faces
// ---------------------------------------------------------------------------

namespace {

bool useLanguage(const std::string& code) {
    if (!cascade::gui::fonts::canDraw(code)) { return false; }
    cascade::i18n::setLanguage(code);
    cascade::gui::fonts::applyLanguage(code);
    cascade::gui::fonts::applyPending();
    return true;
}

// EVERY KEY MUST FIT ITS CARD AT 1234 PX, IN EVERY LANGUAGE. A card is (1188 - 2 x 14) / 3 = 386 px
// wide at that window, its text column 296 px after the glyph and the padding, and the lower row
// holds the one key - whose width follows its word - with, for an UPDATE, "1.10.0 to 1.10.1" in
// amber beside it. The tabs, CHECK AGAIN and the search field share a 900 px top bar.
void testEveryLanguageNewWords() {
    std::printf("  every language: the card keys, the tabs and the top bar fit at 1234 and 900 px\n");
    constexpr float kTextColumn = 296.0f;
    constexpr float kTopBar = 900.0f;
    constexpr float kSearch = 200.0f;
    int cardMiss = 0;
    int barMiss = 0;
    int factMiss = 0;
    int skipped = 0;
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (!useLanguage(l.code)) {
            ++skipped;
            continue;
        }
        float keyW = 0.0f;
        for (const char* w : {"GET", "FITTING...", "INSTALLED", "UPDATE"}) {
            keyW = std::max(keyW, cascade::gui::outlineKeyWidth(cascade::i18n::tr(w)));
        }
        ImFont* rf = cascade::gui::fonts::reading();
        const float px = cascade::gui::fonts::tinyPx() * 1.05f;
        const float versions = rf->CalcTextSizeA(px, FLT_MAX, 0.0f, "1.10.0 to 1.10.1").x;
        // The key, the update's versions beside it and a gap: the widest thing the lower row holds.
        if (keyW + versions + 10.0f > kTextColumn) {
            std::printf("      %s: card key %.0f px + versions %.0f px do not fit %.0f\n", l.code.c_str(),
                        static_cast<double>(keyW), static_cast<double>(versions),
                        static_cast<double>(kTextColumn));
            ++cardMiss;
        }
        // THE DETAILS GRID'S KEY COLUMN (170 px of text room): every fact's name must be drawable in it,
        // at its own size or - drawn smaller, as the page does - at the floor.
        {
            ImFont* lf = cascade::gui::fonts::legend();
            const float tpx = std::max(11.0f, cascade::gui::fonts::tinyPx() * 0.9f);
            for (const char* w : {"MAKER", "LICENCE", "VERSION", "PLUGIN ABI", "DOWNLOAD", "BUILDS FOR", "REACHES",
                                  "HOMEPAGE", "SHA-256", "PUBLISHED", "ON THIS MACHINE", "FILE", "RETIRED BELOW"}) {
                const float fit = cascade::gui::fitTrackedPx(lf, tpx, cascade::i18n::tr(w), 0.14f, 170.0f,
                                                             cascade::gui::fitFloorFor(tpx));
                if (cascade::gui::trackedWidth(lf, fit, cascade::i18n::tr(w), fit * 0.14f) > 170.5f) {
                    std::printf("      %s: fact name \"%s\" does not fit its column even at the floor\n",
                                l.code.c_str(), cascade::i18n::tr(w));
                    ++factMiss;
                }
            }
        }
        // THE TOP BAR at its minimum width: search field, both tabs, CHECK AGAIN and a little of the line.
        const float browse = std::max(cascade::gui::chassisKeyWidth(cascade::i18n::tr("BROWSE")), 100.0f) + 16.0f;
        const float updates = std::max(cascade::gui::chassisKeyWidth("UPDATES (99)"), 120.0f) + 16.0f;
        const float check = cascade::gui::chassisKeyWidth(cascade::i18n::tr("CHECK AGAIN"));
        const float used = 2.0f * 16.0f + kSearch + 24.0f + browse + 8.0f + updates + 18.0f + check + 60.0f;
        if (used > kTopBar) {
            std::printf("      %s: top bar needs %.0f px of %.0f\n", l.code.c_str(), static_cast<double>(used),
                        static_cast<double>(kTopBar));
            ++barMiss;
        }
    }
    useLanguage("en");
    std::printf("    %d card misses, %d top-bar misses, %d fact-name misses, %d languages not drawable here\n",
                cardMiss, barMiss, factMiss, skipped);
    CHECK(cardMiss == 0);
    CHECK(barMiss == 0);
    CHECK(factMiss == 0);
}

}  // namespace

int main() {
    testReachSummary();
    testReachColour();
    testKeyPerInstallState();
    testNoticeGate();
    testBadges();
    testCategories();
    testSearch();
    testUpdatesTab();
    testPageSections();
    testWhatsNew();
    testHeaderMeta();
    testPageFacts();
    testCatalogueLine();
    testColumns();
    testProseSize();
    testAddAllPlan();
    testCatalogueConsentForgotten();
    testCleanupWords();

    // THE REAL FACES for the measured half, as test_bench_text_fits loads them.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);
    if (loaded) {
        ImGui::NewFrame();
        ImGui::Render();
        testEveryLanguageNewWords();
    }
    ImGui::DestroyContext();
    return testSummary("test_plugin_store_view");
}
