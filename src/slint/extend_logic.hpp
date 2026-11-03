// extend_logic.hpp - THE EXTEND BANK'S PURE LOGIC and the PLUGIN STORE PAGE'S (docs/engine-stage4.md, the Slint port, phase 3 bank 4): what the open bodies under the
// EXTEND rows show - Web access and CAT control - and what the Plugin store page shows is computed from the C API table's lists and facts and the state the publisher
// keeps by these functions, and every control the operator touches becomes the exact EFFECT the ImGui window's own control has: an op (with the grant the engine asks
// for), or a local change the publisher keeps, or both. No Slint type and no engine header appears here (foxsdr_api.h, core/app_commands.hpp and the light core headers
// named below), so the file is tested without a window or an engine (tests/test_slint_host_extend.cpp reads a stub table, as the other banks' tests do).
//
// THE CONTRACT is ui/extend_bank.slint's (the UI family's; the note slint-extend-bank-contract.md): the models below are the Slint structs with the same fields in
// snake_case; the publisher (extend_publish.*, kit_publish.cpp for the page, not this file) maps them 1:1 and wires each ExtendEvents callback to the effect function the
// table below names. A field marked "host extra" is not in the contract - the publisher may use it or leave it.
//
// WHERE THE RULES COME FROM (the ImGui window, read for behaviour and for every sentence, never for drawing):
//   Web access   drawWebSection (app_window.cpp 20488) + applyWebSettings / setWebPassword / serverPanel / readServerLive (20286-20412)
//   CAT control  drawCatSection (20414)
//   The store    gui/plugin_store_view.cpp (the pure functions - storeKeyFor, storeBrowseSections, storeCatalogueLine, fillStoreModule, planAddAll, the parsers - and the
//                words the view draws) + app_window_store.cpp (storeLists: the table read, and drawPluginStoreWindow: the requests the view raises and the ops they send)
//                + app_window.cpp drawPluginStoreSection (the rail row's chip)
// and the sentences are those drawers' own, through cascade::i18n::tr() (the catalogues already hold them; this file adds NO key).
//
// plugin_store_view.cpp IS NOT REUSED. Its header includes imgui.h and the unit draws with ImFont, the GL texture cache and the theme, so it does not link without the ImGui
// stack (the kit reuses track_info_cache.cpp because that one includes nothing of ImGui). What is pure in it is MIRRORED here, function for function (the names carry
// over), and what is already pure elsewhere is REUSED: core/add_all_plan.hpp (the GET EVERYTHING rule, header only), core/plugin_store_reasons.cpp (trStoredReason and the
// two reasons), core/plugin_repo_data.cpp (the site address validator and labels), gui/store_first_open.hpp (the one automatic read), core/server_ports.hpp (the port
// range and the address classes). tests/test_slint_host_extend.cpp reads the ImGui sources for every literal this file words.
//
// THE UPDATE ALL KEY IS NOT FOXAPI_OP_STORE_UPDATE_ALL (0x0805). The engine's handler for 0x0805 is startAddAll(on) - the whole ADD ALL run - and the ImGui window's UPDATE ALL
// key sends FOXAPP_OP_STORE_ADD_ALL (0x841E) with ival[1] = 1 (updates only); GET EVERYTHING sends the same op with ival[1] = 0 and ival[0] the tick. This file does what the
// window does.
//
// HOW THE PUBLISHER USES IT: it keeps three drafts (ReadOptions::web / cat / store) and hands them in on every read; an Effect whose local is Web / Cat / Store carries the NEW
// draft whole, which the publisher stores as the next ReadOptions' (the model shows the drafts as it read them, in WebModel::draft / CatModel::draft / StoreModel::draft, so an
// effect starts from what was on screen). It submits each Effect::commands entry on the window's session in order, counts an Ask::ServerConfig (ReadOptions::server_asks_pending,
// down by one when the answer comes - applyServerAnswer puts a refusal into the web draft's error), and after submitting calls scrubEffect so a password does not outlive the send.
// A store read that sets StoreModel::draft_changed (a page the catalogue no longer lists was closed) is adopted as the new draft; watchConsents is called after each store read.
//
// THE CALLBACKS -> EFFECTS (every apply... returns an Effect; "[local]" = no op: the publisher keeps the draft the Effect carries; an empty Effect does nothing, exactly as
// the ImGui control would not; "ask" = the answer the op waits for; SERVER_CONFIG = FOXAPI_OP_SERVER_CONFIG 0x1001 with the JSON text, grant ADMIN):
//
//   web-serve(on)             applyWebServe            [local] WebDraft: enabled = on, dirty. (Nothing is sent until Apply.)
//   web-reach(i)              applyWebReach            [local] WebDraft: 0 = bind 127.0.0.1, 1 = 0.0.0.0, 2 = the specific address kept (offered only while the config holds one), dirty
//   web-port(text)            applyWebPort             [local] WebDraft: port_text = the text typed (clamped to 1024..65535 when it is sent), dirty
//   web-user(text)            applyWebUser             [local] WebDraft: user (63 bytes, the window's buffer), dirty
//   web-password(text)        applyWebPassword         [local] WebDraft: password (127 bytes). NEVER logged, never in an Effect text, never echoed; not "dirty" (as the ImGui box)
//   web-confirm(text)         applyWebConfirm          [local] WebDraft: confirm
//   web-set-password()        applyWebSetPassword      the two boxes differ: [local] WebDraft error "the two passwords do not match" and both boxes wiped. Equal: SERVER_CONFIG
//                                                     {"web":{"enabled","bindAddress","port","username","password":"..."}} (both boxes EMPTY and equal = "clearPassword":true, as the ImGui
//                                                     setWebPassword does), ask ServerConfig, [local] WebDraft: applied (dirty off, error cleared), boxes wiped
//   web-clear-password()      applyWebClearPassword    nothing with no password set; else SERVER_CONFIG {"web":{...,"clearPassword":true}}, ask ServerConfig, [local] applied
//   web-apply()               applyWebApply            nothing unless Apply is enabled; else SERVER_CONFIG {"web":{"enabled","bindAddress","port","username"}}, ask ServerConfig,
//                                                     [local] WebDraft: applied
//   cat-accept(on)            applyCatAccept           SERVER_CONFIG {"cat":{"enabled":on}}, ask ServerConfig, [local] CatDraft enabled = on
//   cat-port(text)            applyCatPort             a COMMIT (the field's accepted / focus-out): SERVER_CONFIG {"cat":{"port":N}} with N the text clamped to 1024..65535; text that is no number sends
//                                                     nothing; ask ServerConfig, [local] CatDraft port_text = N
//   cat-reachable(on)         applyCatReachable        SERVER_CONFIG {"cat":{"bindAll":on}}, ask ServerConfig, [local] CatDraft bind_all = on
//   (the engine's answer to a SERVER_CONFIG: applyServerAnswer(WebDraft&, status, message) - a status other than OK / NO_CHANGE becomes the web error line; the publisher counts the asks)
//
//   store-search(text)        applyStoreSearch         [local] StoreDraft: search = text (127 bytes), page closed ("typing is looking for something")
//   store-tab(i)              applyStoreTab            [local] StoreDraft: tab = 0 BROWSE / 1 UPDATES, page closed
//   store-check()             applyStoreCheck          FOXAPI_OP_STORE_FETCH 0x0801; nothing while a transfer or a picture is in flight or the catalogue url is empty
//   store-sites-toggle()      applyStoreSitesToggle    [local] StoreDraft: sites_open flips
//   store-site-draft(text)    applyStoreSiteDraft      [local] StoreDraft: site_draft = text (511 bytes), site_error cleared
//   store-consent(on)         applyStoreConsent        [local] StoreDraft: consent = on
//   store-site-add()          applyStoreSiteAdd        the key is dead while busy / no address / no consent (nothing). Else the address checked (storeCheckSiteAdd): refused = [local]
//                                                     StoreDraft site_error = the sentence; accepted = FOXAPP_OP_STORE_SITE 0x8709 text = the address as normalised, ival[0] = 0, ival[1] = 1;
//                                                     [local] site_draft = "", consent = false (each site is a fresh decision)
//   store-site-remove(i)      applyStoreSiteRemove     nothing while busy or for an index not listed; FOXAPP_OP_STORE_SITE text = the site's address as listed, ival[0] = 1, ival[1] = 0
//   store-card-open(id)       applyStoreCardOpen       [local] StoreDraft: page_id = id, tab 0, details closed, notice tick off; FOXAPP_OP_STORE_PICTURES_WANT 0x870A text = id, ival[0] = 1
//                                                     (the edge of opening a page asks for its pictures, a failed one again); nothing for an id the catalogue does not list
//   store-card-key(id)        applyStoreCardKey        by the card's key: GET with a legal notice and no tick = opens the page (as store-card-open); GET enabled = FOXAPI_OP_STORE_INSTALL
//                                                     0x0802 text = id, ival[0] = 1 only when id is the open page and its notice is ticked; UPDATE enabled = FOXAPP_OP_STORE_UPDATE 0x8705
//                                                     text = id; FITTING... = FOXAPI_OP_STORE_CANCEL 0x0804 (the contract has no CANCEL key; the window's is in its top bar while busy);
//                                                     INSTALLED, or a dead key = nothing
//   store-get-all()           applyStoreGetAll        nothing unless GET EVERYTHING is enabled; FOXAPP_OP_STORE_ADD_ALL 0x841E text = the module whose own notice tick is on ("" - the key
//                                                     is drawn on the grid, where no page is open), ival[0] = the bulk tick, ival[1] = 0
//   store-bulk-tick(on)       applyStoreBulkTick       [local] StoreDraft: bulk_tick = on
//   store-update-all()        applyStoreUpdateAll      nothing unless UPDATE ALL is enabled; FOXAPP_OP_STORE_ADD_ALL text = "", ival[0] = 0, ival[1] = 1
//   store-update(id)          applyStoreUpdate         as an enabled UPDATE key: FOXAPP_OP_STORE_UPDATE text = id; a FITTING... row = CANCEL; else nothing
//   store-cleanup()           applyStoreCleanup        [local] StoreDraft: cleanup_open (the one confirmation); nothing with no old copy or while busy
//   store-cleanup-confirm(b)  applyStoreCleanupConfirm [local] cleanup_open closes; true: FOXAPP_OP_STORE_CLEAN_OLD 0x870B ival[0] = 0 (every copy the engine's own scan calls superseded NOW)
//   store-plugin-back()       applyStorePluginBack     [local] StoreDraft: page closed, tab 0
//   store-plugin-key()        applyStorePluginKey      the page key, as the card's with the page's own tick (GET is dead until the notice is ticked)
//   store-plugin-details(on)  applyStorePluginDetails  [local] StoreDraft: details_open = on
//   store-picture(i)          applyStorePicture        nothing (the ImGui strip has no press; the page asks for a failed picture again each time it OPENS)
//   (HOST EXTRAS, not contract callbacks:)
//   -                         applyStoreNoticeTick     [local] StoreDraft: notice_tick = on, for the open page only. THE CONTRACT HAS NO CALLBACK FOR THIS TICK and the page's GET key is dead
//                                                     without it for a plugin that carries a legal notice: the UI family adds store-notice(bool)
//   -                         applyStoreCancel         FOXAPI_OP_STORE_CANCEL 0x0804 while busy (the window's CANCEL key)
//   -                         applyStoreOpened         the page opened: FOXAPI_OP_STORE_FETCH once per session when no catalogue was read and nothing is in flight (storeShouldCheckOnOpen)
//   -                         applyStorePicturesTop    the open page lists a picture nobody asked for (state None): FOXAPP_OP_STORE_PICTURES_WANT text = page, ival[0] = 0; the publisher
//                                                     sends at most one for a move of listSeq, as the window does
//
// THE CONSENTS ARE CLEARED BY WATCHING THE TABLE (watchConsents), as the window's storeLists does: a catalogue fetch beginning clears the page tick and the bulk tick; the ADD ALL run
// ending, or a new summary line, clears the bulk tick; a different catalogue from the one read last clears both.
//
// WHAT IS NOT HERE: REMOVE of an installed plugin (the Fitted modules page's key, the kit's - the ImGui store window has none), the retired rows (the kit's, kit_logic BlockedRow),
// the catalogue url box (the kit's setCatalogueUrl) and the picture decode (the publisher loads StorePicture::path; a file it cannot read is "the picture file cannot be read").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/app_commands.hpp"
#include "kit_logic.hpp"
#include "signal_logic.hpp"

namespace foxsdr::slintextend {

using foxsdr::slintkit::TableReader;
using foxsdr::slintsignal::Command;
using foxsdr::slintsignal::SelectItem;
using foxsdr::slintsignal::Tone;

// ---- the constants the ImGui window states ----
constexpr int kMinPort = 1024, kMaxPort = 65535;               // core/server_ports.hpp (a test reads it)
constexpr std::size_t kUserBytes = 63;                         // webUserBuf_[64]
constexpr std::size_t kPasswordBytes = 127;                    // webPassBuf_[128], webPassConfirmBuf_[128]
constexpr std::size_t kSearchBytes = 127;                      // PluginStoreDeck::search[128]
constexpr std::size_t kSiteUrlBytes = 511;                     // PluginStoreDeck::siteUrl[512]
constexpr std::size_t kMaxSites = 8;                           // AppConfig::kMaxPluginSites
constexpr std::size_t kCleanupListed = 3;                      // drawCleanupFoot: "at most three" lines beside the key, then "and %zu more"

// ---- the drafts: what the publisher keeps between reads (the ImGui window's webStage*_, catEnabled_, PluginStoreDeck) ----
struct WebDraft {
    bool have = false;                     // the publisher holds a staged copy (every effect that carries one sets it)
    bool dirty = false;                    // webDirty_: an edit waits for Apply
    bool enabled = false;                  // webStageEnabled_
    std::string bind = "127.0.0.1";        // webStageBind_
    std::string port_text;                 // what the port field holds
    std::string user = "admin";            // webUserBuf_
    std::string password, confirm;         // webPassBuf_, webPassConfirmBuf_ - NEVER logged, never echoed
    std::string error;                     // webError_: a complaint of this window's own, or the engine's refusal of a SERVER_CONFIG
};
struct CatDraft {
    bool have = false;
    bool enabled = false;                  // catEnabled_
    bool bind_all = false;                 // catBindAll_
    std::string port_text;                 // catPortMirror_
};
struct StoreDraft {
    std::string search;
    int tab = 0;                           // 0 BROWSE, 1 UPDATES
    std::string page_id;                   // the plugin whose page is open ("" = the grid): the CATALOGUE KEY
    bool details_open = false;             // ModulePageState::showDetails
    bool notice_tick = false;              // PluginStoreDeck::legalAck - belongs to the open page
    bool bulk_tick = false;                // PluginStoreDeck::addAllAck
    bool sites_open = false;
    std::string site_draft;
    bool consent = false;                  // siteConsent
    std::string site_error;                // the last ADD's refusal (the view's own sentence)
    bool cleanup_open = false;             // the one confirmation is showing
};

// ---- the models (ui/extend_bank.slint's structs, snake_case) ----
struct WebModel {
    bool serve = false;
    std::vector<SelectItem> reach;         // "This machine only", "Every network interface", and "A specific address" only while the config holds one
    int reach_index = 0;
    std::string address;                   // "address: 192.168.1.5" while the specific address is chosen, else ""
    std::string port;
    std::string user;
    std::string password_status;           // "a password is set" / "no password set"
    std::string password_draft, confirm_draft;
    std::string set_label, clear_label;    // "Set password"; "Clear password" ("" with no password set: the key is not drawn)
    std::string apply_label;               // "Apply" / "Applied"
    bool apply_enabled = false;
    std::string error;                     // the red line: this window's complaint, else the refusal a start was given
    std::string serving_note;              // "serving at http://127.0.0.1:8073" / "serving on port N to every machine on your network" / "not serving"
    std::string network_warning;           // "reachable from your network" while the server is on and reachable off this machine
    std::vector<std::string> addresses;    // "http://192.168.1.5:8073" each
    std::string http_note;                 // the plain-HTTP sentence (same condition)
    std::string sessions;                  // "3 browser session(s)" while running and a login is required
    std::string listeners;                 // "1 listening to audio" while running and someone is
    // ---- host extras ----
    std::string chip;                      // the rail row's chip: ON / FAIL / OFF
    Tone chip_tone = Tone::Good;           // Err (the rust lamp) while refused
    bool lit = false;                      // running or refused
    bool running = false, fault = false, reachable = false, auth_required = false, has_password = false;
    bool clear_enabled = false;
    int listening_port = 0;                // SERVERS value[0]
    int clients = 0, audio_listeners = 0;
    unsigned long long commands = 0;
    std::string bind_address;              // the staged bind address, raw
    int bind_choice = 0;                   // 0 this machine only, 1 every interface, 2 a specific address
    int configured_port = 0;               // fact.web.port
    WebDraft draft;                        // what the model shows, as a draft: the effects start from it
};

struct CatModel {
    std::string intro;
    bool accept = false;
    std::string port;
    bool reachable = false;                // "Reachable from other machines" (bindAll)
    std::string warning;                   // "This protocol has no password..." while reachable is ticked
    std::string status;                    // the refusal / "Listening on 127.0.0.1:4532 (em dash) 1 client(s), 12 command(s)" / "Not listening."
    Tone status_tone = Tone::Neutral;      // Err for the refusal
    // ---- host extras ----
    std::string chip;                      // FAIL / OFF / "2 CLI" / ON
    Tone chip_tone = Tone::Good;
    bool lit = false;
    bool running = false, fault = false;
    int clients = 0, listening_port = 0;
    unsigned long long commands = 0;
    CatDraft draft;
};

struct StoreBadge {
    std::string text;
    Tone tone = Tone::Neutral;             // THIRD PARTY and EXPERIMENTAL amber (Warn), a build badge muted (Neutral)
};

// The key kinds of a card, a row and the page (the contract's key-kind).
enum KeyKind : int { kKeyGet = 0, kKeyUpdate = 1, kKeyFitting = 2, kKeyInstalled = 3, kKeyNone = 4 };

struct StoreCard {
    std::string id;                        // the CATALOGUE KEY ("<id>@<site label>" for a site's plugin)
    std::string glyph;                     // the catalogue's category id ("aircraft", "marine", "satellites-weather", "meters-paging", "broadcast", "maps-tools", or "")
    std::string name;
    std::string summary;
    std::vector<StoreBadge> badges;
    std::string key_label;                 // GET / UPDATE / FITTING... / INSTALLED
    int key_kind = kKeyGet;
    bool key_enabled = false;
    std::string version_note;              // "1.0.0 to 1.1.0" beside an UPDATE key
    std::string progress;                  // "downloading <name>" while this card's key is FITTING...
    float progress_norm = 0.0f;            // the transfer's figure 0..1 (0: the server sent no length - the line sweeps)
    // ---- host extras ----
    std::string key_reason;                // why a dead key is dead (translated), the key's tooltip
    bool opens_page = false;               // GET on a plugin with a notice opens its page
    std::string category_word;             // the heading it sits under
};
struct StoreCategory {
    std::string heading;
    std::vector<StoreCard> cards;
    std::string census;                    // "aircraft", ... "other"
};
struct StoreSite {
    std::string address;                   // the display line (host + path, cut to 90)
    std::string remove_label;              // "REMOVE"
    // ---- host extras ----
    std::string url;                       // exactly as configured: what REMOVE names
    bool remove_enabled = true;            // no transfer in flight
    std::string remove_tooltip;            // "Forgets this site..." / "a transfer is already in progress"
};
struct StoreLine {
    std::string text;
    Tone tone = Tone::Neutral;
};
struct UpdateRow {
    std::string id;
    std::string glyph;
    std::string name;
    std::string version_note;              // "1.0.0 to 1.1.0"
    std::string badge;                     // "THIRD PARTY" or ""
    std::string whats_new;                 // the catalogue's paragraph, else the update plan's reason
    std::string key_label;                 // UPDATE / FITTING...
    bool key_enabled = false;
    // ---- host extras ----
    int key_kind = kKeyUpdate;
    std::string key_reason;
    std::string progress;
    float progress_norm = 0.0f;
};

// One screenshot frame of a plugin's page.
enum PictureWord : int { kPicNone = 0, kPicPending = 1, kPicReady = 2, kPicFailed = 3 };
struct PictureFrame {
    std::string sha256;                    // the cache key: what the publisher names the image by
    std::string caption;
    int state = kPicNone;
    std::string path;                      // Ready: the cached file the publisher loads
    std::string word;                      // the frame's own words when there is no image: "loading...", the failure reason, "No pictures published yet"
};
struct PageFact {
    std::string key, value;
    bool hatched = false;                  // no source: lettered over a hatch with the reason
    bool copyable = false;
    Tone tone = Tone::Neutral;
};
struct StorePluginPage {
    bool shown = false;
    std::string back_label;                // "< BROWSE"
    std::string glyph;
    std::string name;
    std::string version;                   // the line under the name: "maker (middle dot) version 1.1.0 (middle dot) 412 kB (middle dot) windows/x64 (middle dot) from host"
    std::vector<StoreBadge> badges;
    std::string origin;                    // "Offered by foxsdr.com in some countries only." / "From a site you added: host/path"
    std::string key_label;
    int key_kind = kKeyGet;
    bool key_enabled = false;
    std::string report;                    // the install report / error of THIS plugin (directly under the header)
    Tone report_tone = Tone::Good;         // Err for an error
    std::vector<PictureFrame> screenshots;
    std::string what_it_does;
    std::string whats_new;
    std::string details;                   // the facts grid as "KEY: value" lines (the declared-not-enforced lead after it while open)
    bool details_open = false;
    std::string details_label;             // "SHOW DETAILS" / "HIDE DETAILS"
    // ---- host extras ----
    std::string id;
    std::string meta;                      // (the same line as `version`)
    std::string version_note;              // "1.0.0 to 1.1.0" beside an UPDATE key
    std::string key_reason;
    std::string progress;
    float progress_norm = 0.0f;
    std::string notice_heading, notice;    // "BEFORE YOU FIT IT" and the legal notice, verbatim, while the plugin is not fitted
    bool notice_shown = false;
    bool notice_tick = false;
    std::string notice_tick_label;         // "I have read the notice above and accept responsibility"
    std::vector<PageFact> facts;
    std::string reach_lead;                // "Declared by the maker, not enforced..." under the open grid
    std::string screenshots_heading, what_heading, new_heading, details_heading;   // SCREENSHOTS, WHAT IT DOES, WHAT'S NEW, DETAILS
};

struct StoreModel {
    std::string search;
    std::string search_placeholder;
    int tab = 0;
    std::string browse_label, updates_label;
    std::string catalogue_line;
    Tone catalogue_tone = Tone::Neutral;   // Warn for "Catalogue from <date>; could not refresh: <reason>"
    std::string check_label;               // "CHECK NOW" until the catalogue was asked for, then "CHECK AGAIN"
    bool sites_open = false;
    std::vector<StoreSite> sites;
    std::string no_sites_line;             // "No sites added."
    std::string site_draft;
    bool consent = false;
    std::string consent_sentence;
    std::string add_site_label;            // "ADD"
    std::string top_error;                 // the install error of the last transfer, wherever the user is: "<name>: <reason>"
    std::string top_warning;               // a catalogue that loaded with a warning
    std::string empty_line;                // why there is nothing to list
    std::vector<StoreCategory> categories;
    std::string get_all_label;             // "GET EVERYTHING" ("" when the key is not drawn: no catalogue rows, or a search is typed)
    bool get_all_enabled = false;
    std::string get_all_reason;            // why the key is dead (translated), "" while it is live
    std::string foot_note;                 // the sentence beside the key: the run's progress, "Cannot add all: ...", or "%d to fetch and %d to update..."
    bool bulk_tick = false;
    std::string bulk_sentence;             // "I accept the 2 legal notices on their plugins' pages" ("" when no notice modules or a run is going)
    std::string updates_heading;           // "UPDATES AVAILABLE" / "NO UPDATES"
    std::string update_all_label;          // "UPDATE ALL" ("" when there is nothing to update: the key is not drawn)
    bool update_all_enabled = false;
    std::vector<UpdateRow> updates;
    std::vector<std::string> footnotes;    // the UPDATES tab's notes: "Updates are fetched when you press CHECK AGAIN...", the sites-left-out note
    std::string cleanup_label;             // "CLEAN UP OLD VERSIONS (3)" ("" with no old copy)
    bool cleanup_enabled = false;
    std::vector<std::string> cleanup_lines;   // the first three old copies, then "and N more"
    bool cleanup_open = false;
    std::string cleanup_title, cleanup_text, cleanup_yes, cleanup_no;
    StorePluginPage plugin;
    // ---- host extras ----
    bool busy = false;                     // a catalogue read or a download is in flight
    std::string busy_label;                // "fetching the catalogue" / "downloading <name>"
    std::string cancel_label;              // "CANCEL" while busy
    bool check_enabled = false;
    std::string sites_label;               // "SITES"
    std::string site_placeholder;          // "index.json URL"
    std::string site_error;                // the last ADD's refusal
    bool add_site_enabled = false;
    std::string add_site_reason;           // why ADD is dead
    std::vector<StoreLine> site_lines;     // one status line a site (two or three when the merge hid something)
    Tone empty_tone = Tone::Neutral;       // Err for "The catalogue could not be read: ..."
    Tone top_error_tone = Tone::Err;
    bool bulk_shown = false;               // the combined tick is drawn
    std::string notice_note;               // "2 of these carry a legal notice from their maker: A, B. Each notice is on that plugin's page."
    std::string left_out_note;             // the sites-left-out sentence under GET EVERYTHING
    std::string add_all_summary;           // what the last run did
    bool add_all_failed = false;
    bool add_all_running = false;
    std::string cleanup_report;            // what the last clean-up did, green
    std::vector<std::string> cleanup_all;  // every old copy, for the confirmation
    int update_count = 0;
    std::string source_url;                // fact.store.url (the kit's catalogue box)
    bool have_catalogue = false;           // CATALOGUE is not empty
    bool read_this_session = false;        // fact.store.readThisSession: a list read THIS session (what the first open asks)
    bool picture_busy = false;             // a picture is queued or in flight: the one transfer slot is held
    int busy_kind = 0;                     // 1 the catalogue, 2 a download
    std::vector<std::string> ids;          // every catalogue key listed
    std::vector<std::string> site_urls;    // the sites as configured
    std::uint64_t catalogue_hash = 0;      // what the catalogue is (the consents' edge)
    bool hash_valid = false;
    StoreDraft draft;                      // what the model shows, as a draft
    bool draft_changed = false;            // the read changed the draft (a page the catalogue no longer lists was closed): the publisher adopts `draft`
};

// The rows' chips: Web access, CAT control (the EXTEND rail rows).
enum Row : int { kRowWeb = 0, kRowCat };
constexpr int kExtendRowCount = 2;
struct RowChip {
    std::string word;
    Tone tone = Tone::Good;
    bool lit = false;
};

struct ExtendModels {
    WebModel web;
    CatModel cat;
    StoreModel store;
    std::array<RowChip, kExtendRowCount> chips;
};

// The DECODE bank's "Plugin store" row (drawPluginStoreSection): BUSY / IDLE / "N UPD" / OK.
struct StoreRailChip {
    std::string word;
    Tone tone = Tone::Good;
    bool lit = false;
};

// ---- what the publisher owns and hands in each time ----
struct ReadOptions {
    WebDraft web;
    CatDraft cat;
    StoreDraft store;
    int server_asks_pending = 0;           // serverAsksPending_: SERVER_CONFIGs sent and not yet answered (the mirrors are not taken from the facts meanwhile)
    bool read_store = true;                // false: the Web and CAT bodies only (the store page is not open): the store model holds its draft and nothing else
    std::int64_t now_unix = 0;             // the clock the catalogue line is read against (std::time(nullptr))
};

// ---- the reads ----
WebModel readWeb(TableReader& table, const ReadOptions& options);
CatModel readCat(TableReader& table, const ReadOptions& options);
StoreModel readStore(TableReader& table, const ReadOptions& options);
ExtendModels readExtend(TableReader& table, const FoxReceiverState& state, const ReadOptions& options);
StoreRailChip readStoreChip(TableReader& table);

// ---- the store's pure rules, mirrored from gui/plugin_store_view.cpp (the names carry over) ----
enum class Category { Aircraft, Marine, SatellitesWeather, MetersPaging, VoiceData, MapsTools, Other };
constexpr int kCategoryCount = 7;
Category categoryFor(const std::string& catalogueId);
std::string categoryHeading(Category c);
const char* categoryCensusName(Category c);

enum class InstallKind { NotInstalled, Installed, UpdateAvailable, NewerInstalled };
struct StoreEntry {                         // PluginStoreModel's StoreModule + ModulePlate, the parts the page uses
    std::string id, name, version, maker, licence, blurb, summary, homepage, legal_notice, origin_note, file_name;
    std::string category, whats_new, published, sha256, platforms, floor_version;
    bool experimental = false, fitted = false, loaded = false, running = false;
    bool have_capabilities = false, have_tune_grant = false, tune_granted = false;
    std::uint32_t capabilities = 0;
    bool have_size = false;
    std::uint64_t size_bytes = 0;
    bool have_abi = false;
    std::uint32_t abi_version = 0, host_abi_version = 0;
    bool have_descriptor = true;
    bool third_party = false;
    std::string site_host;
    bool installable_here = false, have_build_here = false, builds_windows = false, builds_linux = false;
    InstallKind install = InstallKind::NotInstalled;
    std::string installed_version;
    std::string blocked_reason, blocked_reason_if_acknowledged;
    std::string update_to_version, update_reason, update_blocked_reason;
    std::vector<PictureFrame> pictures;     // the entry's pictures with the table's state
};
struct EntryKey {
    int kind = kKeyGet;
    bool enabled = false;
    std::string reason;                    // English, as the gate says it (translate with trStoredReason when drawn)
    bool opens_page = false;
};
struct KeyIn {
    std::string busy_id;                   // the plugin being fitted ("" for a catalogue read or nothing)
    bool busy_any = false;
    bool on_page = false;
    bool notice_ticked = false;
};
EntryKey keyFor(const StoreEntry& e, const KeyIn& in);
const char* keyLabel(int kind);            // GET / UPDATE / FITTING... / INSTALLED (translated)
bool matchesQuery(const StoreEntry& e, const std::string& lowerQuery);
std::vector<int> updateRows(const std::vector<StoreEntry>& entries, const std::string& lowerQuery);
std::string lowerAscii(const std::string& s);
std::string fromTo(const std::string& from, const std::string& to);
std::string headerMeta(const StoreEntry& e, const std::string& hostPlatform);
std::string bytesText(std::uint64_t bytes);
std::string whatsNewText(const StoreEntry& e);
std::vector<PageFact> pageFacts(const StoreEntry& e);
std::string reachesLine(const StoreEntry& e);
std::string cleanupKeyLabel(std::size_t count);
std::string oldCopyLine(const std::string& name, const std::string& version, const std::string& file, const std::string& keptVersion);
std::string cleanupConfirmLabel(std::size_t count);
std::string busyLabel(int kind, const std::string& name);
std::string addAllLine(std::uint64_t position, std::uint64_t total, const std::string& name);
// The sentence under the SITES panel's field, and the two "left out" notes.
std::string siteConsentText();
std::string sitesLeftOutNote();
// May this address be added? "" when it may (and `normalised` is what to send), else the sentence.
std::string checkSiteAdd(const std::string& raw, const std::vector<std::string>& siteUrls, std::size_t maxSites, const std::string& officialUrl, std::string& normalised);
// The catalogue line and its tone (storeCatalogueLine), against a clock.
StoreLine catalogueLine(bool haveCatalogue, bool fromCache, bool refreshFailed, const std::string& sourceError, std::int64_t readTime, std::int64_t nowUnix);

// ---- the consents (watchConsents) ----
struct ConsentWatch {                       // what the window's StoreListCache keeps for the edges
    bool add_all_was_running = false;
    std::string last_add_all_summary;
    bool have_hash = false;
    std::uint64_t hash = 0;
};
struct ConsentEdge {
    bool clear_notice_tick = false;
    bool clear_bulk_tick = false;
    bool any() const { return clear_notice_tick || clear_bulk_tick; }
};
// Called by the publisher after each store read with the model it got; the edge it returns clears the matching ticks in the draft it holds.
ConsentEdge watchConsents(ConsentWatch& watch, const StoreModel& model);

// ---- the effects ----
enum class Local {
    None,
    Web,                                   // `web` is the new draft
    Cat,                                   // `cat`
    Store,                                 // `store`
};
// The answer an op waits for.
enum class AskKind { None, ServerConfig };
struct Ask {
    AskKind kind = AskKind::None;
};
struct Effect {
    std::vector<Command> commands;         // ops to submit on the window's session, in order
    Local local = Local::None;
    WebDraft web;
    CatDraft cat;
    StoreDraft store;
    Ask ask;
    bool pictures_asked = false;           // a STORE_PICTURES_WANT went out for listSeq: the publisher does not send the top-up again for it
    bool empty() const { return commands.empty() && local == Local::None; }
};

Effect applyWebServe(const WebModel& m, bool on);
Effect applyWebReach(const WebModel& m, int index);
Effect applyWebPort(const WebModel& m, const std::string& text);
Effect applyWebUser(const WebModel& m, const std::string& text);
Effect applyWebPassword(const WebModel& m, const std::string& text);
Effect applyWebConfirm(const WebModel& m, const std::string& text);
Effect applyWebSetPassword(const WebModel& m);
Effect applyWebClearPassword(const WebModel& m);
Effect applyWebApply(const WebModel& m);
Effect applyCatAccept(const CatModel& m, bool on);
Effect applyCatPort(const CatModel& m, const std::string& text);
Effect applyCatReachable(const CatModel& m, bool on);
// The engine's answer to a SERVER_CONFIG (fox_session answer: status, message): the error line, as the window sets webError_.
void applyServerAnswer(WebDraft& draft, int status, const std::string& message);
// Overwrites what an effect carries of a password (the commands' text and long text, the draft's boxes), as applyWebSettings does after the send.
void scrubEffect(Effect& e);
// The JSON text a SERVER_CONFIG carries (public for the tests).
std::string webConfigJson(const WebDraft& staged, const std::string& extraMembers, int fallbackPort);
std::string jsonQuote(const std::string& s);

Effect applyStoreSearch(const StoreModel& m, const std::string& text);
Effect applyStoreTab(const StoreModel& m, int tab);
Effect applyStoreCheck(const StoreModel& m);
Effect applyStoreSitesToggle(const StoreModel& m);
Effect applyStoreSiteDraft(const StoreModel& m, const std::string& text);
Effect applyStoreConsent(const StoreModel& m, bool on);
Effect applyStoreSiteAdd(const StoreModel& m);
Effect applyStoreSiteRemove(const StoreModel& m, int index);
Effect applyStoreCardOpen(const StoreModel& m, const std::string& id);
Effect applyStoreCardKey(const StoreModel& m, const std::string& id);
Effect applyStoreGetAll(const StoreModel& m);
Effect applyStoreBulkTick(const StoreModel& m, bool on);
Effect applyStoreUpdateAll(const StoreModel& m);
Effect applyStoreUpdate(const StoreModel& m, const std::string& id);
Effect applyStoreCleanup(const StoreModel& m);
Effect applyStoreCleanupConfirm(const StoreModel& m, bool yes);
Effect applyStorePluginBack(const StoreModel& m);
Effect applyStorePluginKey(const StoreModel& m);
Effect applyStorePluginDetails(const StoreModel& m, bool on);
Effect applyStorePicture(const StoreModel& m, int index);
Effect applyStoreNoticeTick(const StoreModel& m, bool on);
Effect applyStoreCancel(const StoreModel& m);
struct StoreFirstOpenState { bool settled = false; };
Effect applyStoreOpened(const StoreModel& m, StoreFirstOpenState& first, bool readThisSession);
Effect applyStorePicturesTop(const StoreModel& m);

}  // namespace foxsdr::slintextend
