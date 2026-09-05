// Persistent application configuration: one plain aggregate, serialized as
// JSON. The aggregate carries the current app defaults in its member
// initializers so "missing field" and "fresh install" are the same code path:
// start from AppConfig{} and overwrite only what the file provides.
//
// Load semantics (all documented here because callers depend on them):
//   - missing file            -> out = defaults, returns true (first run is
//                                not an error; nothing to report)
//   - unreadable / corrupt /  -> out = defaults, returns false, error says why
//     non-object root
//   - schemaVersion mismatch  -> out = defaults, returns false (a future
//     (present but != 1)         schema may renumber/reinterpret fields, so
//                                trusting any of them would be a guess)
//   - unknown fields          -> ignored (forward compatibility)
//   - missing fields          -> keep their defaults
//   - wrong-typed fields      -> that field keeps its default, the rest load
//                                normally, returns true
//
// Range sanitization on load (bad values are repaired, not rejected — a
// hand-edited file should degrade gracefully, never brick the GUI):
//   - volume      clamped to [0, 1]        (sink contract)
//   - splitRatio  clamped to [0.1, 0.9]    (either panel collapsing to zero
//                                           height makes the divider
//                                           ungrabbable — the user could
//                                           never recover by mouse)
//   - dbMin/dbMax must satisfy dbMin < dbMax - 10; otherwise BOTH reset to
//     defaults (the display maps dB to pixels via 1/(dbMax-dbMin); a
//     degenerate or inverted span is a divide-by-zero, and clamping only one
//     end would invent a range the user never chose)
//   - sourceKind  must be "siggen" | "file" | "soapy"; anything else resets
//     to "siggen" (the only source that can never fail to exist)
//   - deemphasisIndex clamped to [0, 2]  (the three-entry 50 us / 75 us / off
//                                         combo; an out-of-range index would
//                                         read past that table)
//   - nrStrength  clamped to [0, 1]      (module contract)
//   - notchFreqHz clamped to [10, 20000] (audible span inside the 48 kHz
//                                         sink's Nyquist; the Notch clamps
//                                         again internally, but a NaN or a
//                                         negative from a hand-edited file
//                                         should never reach a slider)
//   - notchQ      clamped to [0.1, 1000] (the Notch's own useful range)
//   - pluginCatalogueUrl: an EMPTY value resets to PluginRepo::defaultIndexUrl().
//     Anything else is kept verbatim — deliberately, because this field is the
//     enterprise escape hatch (point the browser at your own catalogue) and the
//     component that consumes it already refuses everything it must refuse: a
//     non-https URL never reaches the transport (PluginRepo rule 1), and every
//     download URL inside the fetched index is re-checked there too. Validating
//     the string here as well would only add a second, weaker copy of a rule
//     that has exactly one enforcement point today.
//   - pluginLastUpdateCheck: a negative value resets to 0 ("never"). A time
//     before the epoch is either a hand-edit or a clock that went backwards,
//     and "never checked" is the only honest reading of either.
//   - pluginTuneAllowed: a non-array resets to empty (no grants), which is the
//     same state a fresh install has. Empty and duplicate entries are dropped,
//     and the list is capped at kMaxTuneGrants — it is a list of plugin names
//     the user ticked, so a file claiming thousands is a hand-edit and the cap
//     stops it becoming a per-tune-request linear scan of arbitrary length.
//     A name that matches no installed plugin is KEPT: a grant must survive a
//     plugin being temporarily removed or renamed aside by the retirement
//     quarantine, or reinstalling would silently re-grant it.
//   - pluginsStopped: the SAME rules, applied by the same code, because it is
//     the same shape of list keyed on the same module file names. A stop the
//     file could not express (an empty name) or states twice (a duplicate) is
//     noise from a hand-edit, and a stopped plugin that is not installed right
//     now must stay stopped for when it comes back.
//   - pluginMuteOverride: the same rules again, from the same code. It lists
//     the plugins whose "mute audio while running" setting DIFFERS from the
//     default their capabilities imply, so a duplicate would be a preference
//     that flipped twice and an empty name a preference for nothing.
//
// Save semantics: ATOMIC. The JSON is written to a temp file in the target's
// directory, then renamed over the target, so a crash, full disk, or locked
// target leaves either the complete old config or the complete new one on
// disk — never a truncated hybrid. Parent directories are created on demand.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// For PluginRepo::defaultIndexUrl(), which IS the default value of
// pluginCatalogueUrl below. Spelling the default as a call rather than as a
// copied string literal is deliberate: the catalogue origin is a security
// -relevant constant, and two copies of it that can drift is exactly how a
// build ends up quietly pointing at the wrong host.
#include "core/plugin_repo.hpp"

namespace cascade::core {

struct AppConfig {
    int schemaVersion = 1;
    std::string sourceKind = "siggen";      // "siggen" | "file" | "soapy"
    // RX antenna port for a Soapy device, e.g. "TX/RX" or "RX2" on a B200.
    // Empty means "whatever the driver defaults to", which is what every
    // pre-existing config will say. It is persisted because the port is a
    // property of how the radio is CABLED, not of a session: a user who has
    // an antenna on TX/RX must not have to reselect it every launch, and
    // picking the wrong one produces a working-looking receiver that hears
    // essentially nothing.
    std::string soapyAntenna;
    std::string soapyArgs;                  // kwargs of the last soapy device
    std::string iqFilePath;
    double centerHz = 100000000.0;
    std::string mode = "WFM";
    double bandwidthHz = 150000.0;
    float squelchDb = -120.0f;
    float volume = 0.5f;
    float dbMin = -110.0f, dbMax = 0.0f;
    float splitRatio = 0.4f;
    double vfoOffsetHz = 300000.0;
    double sampleRateHz = 2000000.0;

    // --- P7 feature settings --------------------------------------------------
    // Defaults are the app's own defaults, chosen so an existing installation
    // sounds and looks exactly as it did before these fields existed: every
    // audio processor OFF (noise reduction, notch, auto-notch), de-emphasis
    // on the global 50 us default. The two that default ON are the ones whose
    // "off" state would be a missing feature rather than a preserved
    // behaviour: broadcast stereo decoding (inert outside WFM, and inert
    // inside it until a real 19 kHz pilot locks) and the band-plan overlay
    // (inert unless band plan files are actually installed).
    bool stereoEnabled = true;
    int deemphasisIndex = 0;  // 0 = 50 us, 1 = 75 us, 2 = off (kDeemphUs order)
    bool nrEnabled = false;
    float nrStrength = 0.5f;
    bool notchEnabled = false;
    double notchFreqHz = 1000.0;
    double notchQ = 30.0;
    bool autoNotch = false;
    bool bandPlanOverlay = true;

    // --- Map trails -----------------------------------------------------------
    // TWO SWITCHES, because the request behind them was ambiguous and both
    // readings deserve an answer. A beta tester asked for the flight trail to
    // be coloured by altitude the way the aircraft already is, and added that
    // "some people may not want this, so make it a toggle" - which leaves
    // "does not want the colouring" and "does not want the trails" both live
    // readings of one sentence. One switch would have picked for them.
    //
    //   mapTrails               draw the path layer at all. Off hides every
    //                           line a plugin publishes - a flight trail, a
    //                           predicted ground track, a footprint circle -
    //                           because a control that hid two of the three
    //                           kinds of line on the map would be one nobody
    //                           could predict.
    //   mapTrailAltitudeColours colour ALONG a trail, each segment taking the
    //                           band colour of the altitude the host observed
    //                           there. Off leaves the trails drawn in their
    //                           owner's single colour, exactly as they were
    //                           before this existed.
    //
    // BOTH DEFAULT ON, which is the same rule the two display aids above
    // follow: their "off" state would be a missing feature rather than a
    // preserved behaviour, and an install that has never heard of these keys
    // should get the feature that was asked for.
    bool mapTrails = true;
    bool mapTrailAltitudeColours = true;

    // HOW a trail is drawn, not whether: 0 = line, 1 = ribbon. An integer with
    // named values rather than a bool, because a third style costs one entry
    // here instead of a second flag that can contradict the first - two bools
    // can express "line and ribbon at once", which is not a thing.
    //
    // Line is the default deliberately. A ribbon is what the tester asked to
    // have available, but it covers noticeably more of the map underneath, and
    // changing how every existing user's map looks is not something to do to
    // them without being asked. It is one control away.
    int mapTrailStyle = 0;

    // --- The ADS-B radar scope ------------------------------------------------
    // A plan-position indicator - the receiver at the centre, range rings and
    // bearing ticks around it, aircraft plotted at their true range and
    // bearing, and a detail panel for the one selected. It is a MODE of the
    // main window rather than a panel in it: while it is on, the spectrum, the
    // waterfall and the settings rail are not drawn at all.
    //
    // scopeMode  IS PERSISTED, and that is a deliberate choice rather than an
    //            oversight about a "temporary" view. A user who left the
    //            application showing a scope was using it as a scope; an
    //            instrument that reopens as a spectrum analyser every launch is
    //            one they have to switch on every time. It defaults OFF because
    //            the receiver's own controls are what a new install needs to
    //            see first, and because the scope is empty without an aircraft
    //            source installed. The mode always draws its own way out, so
    //            restoring it can never strand anybody.
    //
    // scopeRangeNm  the scale in nautical miles, and it is one of a fixed
    //            LADDER of values (10, 25, 50, 100, 200, 400 - see
    //            gui/scope_view.hpp, which owns that list). CLAMPED ON LOAD to
    //            the nearest of them, because everything the view draws is
    //            derived from this number: four ring radii, four ring labels
    //            and the corner readout. A hand-edited 173 would reach the
    //            renderer as a scale with rings nobody chose, so the sanitizer
    //            snaps it - to the NEAREST rather than back to the default,
    //            which keeps what the edit was reaching for instead of
    //            discarding a number that was almost right. 200 NM is the
    //            default: a good ADS-B site hears 200-250 NM, so it opens
    //            showing everything the receiver can realistically reach.
    bool scopeMode = false;
    int scopeRangeNm = 200;

    // railBank   which of the FUNCTION SELECT rail's five banks was showing -
    //            SIGNAL PATH, DECODE, VIEW, EXTEND, SYSTEM, as 0..4 in that
    //            order (gui/rail_banks.hpp owns the list). Restored so the
    //            rail opens where it was left, exactly as a section's own
    //            open/closed state does; CLAMPED ON LOAD to a bank that exists,
    //            so a hand-edited 7 opens on the last bank rather than on
    //            nothing. 0 is the default: the signal path is where a new
    //            installation has to start.
    int railBank = 0;

    // --- Map window geometry --------------------------------------------------
    // The map is its own operating system window, and ImGui's own .ini
    // persistence is switched off in this application (IniFilename = nullptr,
    // so no stray file appears beside the executable). That left the map
    // opening at a fixed size on every launch no matter what the user had
    // dragged it to the session before, which is the whole of the "it should
    // be resizable" report: it always WAS resizable, the size just never
    // survived.
    //
    // ZERO WIDTH MEANS "NOTHING SAVED", and is the default. It is not a legal
    // window size, so it needs no companion flag, and it is what makes a first
    // run - and a run after the geometry below is rejected - fall back to the
    // size derived from the monitor's work area rather than to a constant that
    // was chosen for somebody else's screen. The POSITION is only honoured
    // when a size was saved with it: the two were written by the same gesture
    // and are one decision, and 0,0 is a perfectly legal position that could
    // not otherwise be told apart from "unset".
    //
    // Sanitized on load as a rectangle, not as four numbers: if ANY of them is
    // outside its documented range the whole geometry is discarded. A
    // half-rejected rectangle is a rectangle nobody chose, which is the same
    // rule dbMin/dbMax already follow.
    //   width/height  kMapWindowMinPx .. kMapWindowMaxPx, or 0 for unset
    //   x/y           -kMapWindowMaxPx .. kMapWindowMaxPx (a second monitor
    //                 may legitimately sit at a negative coordinate)
    //
    // LEGACY, READ-ONLY. Since the map became one page per plugin (mapPages
    // below), these four are still READ so a rectangle saved by an older
    // build seeds the first pages' default placement — but they are never
    // written back. A config saved by this build carries mapPages only.
    int mapWindowWidth = 0;
    int mapWindowHeight = 0;
    int mapWindowX = 0;
    int mapWindowY = 0;

    // --- Per-plugin map pages -------------------------------------------------
    // One entry per plugin map page: which plugin, where its window sits, and
    // whether it was open. The map used to be ONE window fed every plugin's
    // targets at once, which is why switching from Satellites to ADS-B still
    // showed "the satellite map"; each track-capable plugin now gets a page of
    // its own, and each page's rectangle has to survive a restart for exactly
    // the reason the single window's did.
    //
    // The rectangle follows the legacy fields' rules verbatim — zero width
    // means "nothing saved", and an out-of-range component discards the whole
    // rectangle (but only THIS entry's; the entry itself survives with its
    // open flag, falling back to default placement). An entry with an empty
    // plugin name is dropped, duplicates keep the first, and the list is
    // capped — the same hygiene the plugin-name lists get, for the same
    // hand-edit reasons.
    struct MapPage {
        std::string plugin;  // the plugin's display name, as HostTrack::plugin
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool open = false;
        bool operator==(const MapPage&) const = default;
    };
    std::vector<MapPage> mapPages;

    // --- The receiver's own position ------------------------------------------
    // Where the antenna is, in degrees. It is the origin of every
    // receiver-relative number the map produces: the range rings, the range and
    // bearing in the hover readout, the DISTANCE and BEARING columns of the
    // track table, and the coverage map's whole coordinate system.
    //
    // WHY IT IS PERSISTED, AND WHY THAT IS NOT COSMETIC. It used to live in two
    // static locals beside the "Set RX here" button, which meant it was lost on
    // every restart - so a distance column reading from it would have measured
    // from 0N 0E, in the Gulf of Guinea, and reported a few thousand kilometres
    // for an aircraft overhead. An antenna does not move between launches; the
    // number that says where it is should not either.
    //
    // A SEPARATE "IS SET" FLAG, unlike the map geometry above, which uses zero
    // width as its "nothing saved" sentinel. There is no such spare value here:
    // 0.0, 0.0 is a real place on the equator, and a receiver on Null Island
    // must not be told it has no position. The flag is the only honest way to
    // tell "unset" from "set to the origin".
    //
    // NOTHING GUESSES IT. Inferring the receiver's position from a decoded
    // target would be confidently wrong the moment the first aircraft appears,
    // and the coverage map built on that guess would be a picture of the wrong
    // antenna. Unset stays unset until the user says otherwise, and every
    // consumer says "no RX position" rather than showing a number.
    //
    // Sanitized on load as ONE position, the same rule the rectangle above
    // follows: a latitude outside -90..90, a longitude outside -180..180, or a
    // non-finite either - all reachable by hand-editing the file - discards the
    // whole position rather than clamping half of it to a place nobody chose.
    // Coordinates present without the flag are discarded too: a position nobody
    // set is not a position.
    bool rxPositionSet = false;
    double rxLatDeg = 0.0;
    double rxLonDeg = 0.0;

    // --- Plugin browser settings (P9) -----------------------------------------
    // Where the in-app plugin browser looks for its catalogue. Persisted so a
    // user — or an enterprise deploying cascade — can point the browser at
    // their own index.json instead of the published one, without a rebuild.
    //
    // Two forms are accepted by the browser:
    //   - an https:// URL, fetched over the network (PluginRepo::fetchIndex,
    //     with every rule in plugin_repo.hpp applied);
    //   - a plain filesystem path (no "://" scheme), read directly and parsed
    //     with PluginRepo::parseIndex — no network involved at all. This is
    //     what makes a catalogue on a corporate share usable, and it is also
    //     the only way the catalogue UI's success path can be exercised
    //     offline. It grants no new powers: every DOWNLOAD url inside such an
    //     index must still be https, because install() checks it.
    // A URL with any other scheme (http://, ftp://, file://) reaches
    // fetchIndex and is refused there, which is where that decision belongs.
    std::string pluginCatalogueUrl = PluginRepo::defaultIndexUrl();

    // Whether the plugin store window was left open. Purely cosmetic
    // restore of where the user was — it does NOT cause a fetch on startup.
    // Nothing in this product touches the catalogue origin until the user
    // presses CHECK NOW in that window; that promise is what the store's
    // privacy note makes, and a config field that could reinstate a network
    // call behind the user's back would break it.
    bool pluginBrowserOpen = false;

    // --- The fitted modules window --------------------------------------------
    // Whether the FITTED MODULES window was left open, and where it sat.
    //
    // ITS SIBLING ALREADY PERSISTED AND IT DID NOT. The plugin store and the
    // fitted modules window are the same kind of thing — a rail key in DECODE
    // that opens a window of its own — and the store's open state has always
    // been remembered by pluginBrowserOpen above while this one was a plain
    // member with no key at all, so it closed on exit and was gone on the next
    // launch. Two sibling windows behaving differently is a bug in one of
    // them.
    //
    // NOTHING HERE SELF-OPENS ANYTHING. The flag is written only by the user's
    // own two gestures — the rail key and the window's close button — so
    // restoring it puts back a window the user themselves left open. That is
    // not the self-open the satellite page was fixed for: there is no arrival
    // edge on this window, and nothing but a user action can ever set this
    // true. Restoring it starts no scan and no fetch; the module list is what
    // the host already loaded at start-up.
    //
    // The rectangle follows the map pages' rules verbatim — zero width means
    // "nothing saved", the position is only honoured when a size was saved
    // with it, and an out-of-range component discards the whole rectangle
    // (the open flag survives, because "where the window sat" and "whether it
    // was open" are separate decisions and only one of them went bad).
    bool fittedModulesOpen = false;
    int fittedModulesX = 0;
    int fittedModulesY = 0;
    int fittedModulesWidth = 0;
    int fittedModulesHeight = 0;

    // --- Plugin version policy (P10) ------------------------------------------
    // When a plugin catalogue was last successfully seen, in seconds since the
    // Unix epoch; 0 means never. It exists so the UI can say how old the
    // cached retirement policy is ("last checked 3 weeks ago") next to a
    // blocked plugin, which is the difference between an explanation and a
    // mystery.
    //
    // NOTE WHAT IS NOT HERE: there is no pluginAutoUpdate flag, because there
    // is no auto-update. Nothing in this product fetches a catalogue, still
    // less a native DLL, without the user asking. The staleness problem is
    // solved locally instead - PluginRepo caches the catalogue's
    // minSupportedVersion floor and refuses to load anything below it, with or
    // without a network - so keeping users off stale plugins never requires a
    // background download. A "check for updates on launch" setting would be a
    // background network call in a product that promises none, and in the
    // UK/EU an IP address arriving at our server on every launch is personal
    // data being collected by default. This field records when the user last
    // chose to look; it never causes a look.
    //
    // Sanitized on load: a negative value (hand-edit, or a clock that went
    // backwards) resets to 0, i.e. "never", which is the honest reading.
    std::int64_t pluginLastUpdateCheck = 0;

    // --- Plugin tune permission -----------------------------------------------
    // Names of the plugins the user has allowed to move the receiver. Empty by
    // default, and empty is the secure state: PluginUi answers every
    // request_tune with CASCADE_TUNE_DENIED unless the plugin's name is in this
    // list.
    //
    // WHY IT IS PERSISTED AT ALL. A satellite tracker that corrects for Doppler
    // is useless without it, and the grant is a decision about a piece of
    // software the user installed, not about this session — asking again every
    // launch would train them to tick it without reading it, which is the
    // failure mode consent dialogs are famous for. It is stored by NAME rather
    // than by file hash deliberately: a grant that evaporated on every plugin
    // update would be the same nag by another route.
    //
    // Nothing here can grant anything on its own. A name in this list only
    // matters if a plugin by that name is installed AND declares the host-
    // client capability AND actually calls request_tune; the tune itself still
    // goes through PluginUi's range checks and the receiver's own refusal.
    std::vector<std::string> pluginTuneAllowed;

    // --- Plugins the user has STOPPED -----------------------------------------
    // Module FILE NAMES — the same identity pluginTuneAllowed uses, and for the
    // same reason: a display name is the plugin's own to choose, so keying on
    // it would let a plugin inherit another's state by renaming itself.
    //
    // A stopped plugin is still installed, still loaded, and still has a row on
    // the panel; it simply has no runtime instances, so it decodes nothing,
    // puts nothing on the map, opens no window and cannot move the receiver.
    //
    // WHY IT IS PERSISTED. Stopping a plugin is what a user does about a
    // decoder that is noisy, expensive, or wrong for the band they are on, and
    // a stop that lasted only until the next launch — or, worse, only until the
    // next rescan or source change — would be indistinguishable from a bug.
    // Empty by default: nothing is stopped until somebody stops it.
    //
    // Sanitized on load exactly like pluginTuneAllowed (see the header note):
    // a non-array resets to empty, empties and duplicates are dropped, and the
    // list is capped. A name that matches no installed plugin is KEPT, so a
    // stop survives the plugin being quarantined by the retirement policy or
    // temporarily removed.
    // WINDOWS THE USER HAS CLOSED, remembered across launches.
    //
    // A plugin-declared panel and a decoded-image window used to open on
    // every launch and a close only held for the session, so a user who does
    // not want their satellite tracker's list on screen had to shut it again
    // every single time they started the application - reported exactly that
    // way. The entries are ImGui window identities ("Satellites###panel_
    // Satellites"); an identity that no longer exists is simply never
    // matched, which is what makes an uninstalled plugin's leftovers
    // harmless. Capped like every other list here so a corrupt or hostile
    // file cannot grow the config without bound.
    std::vector<std::string> closedWindows;

    std::vector<std::string> pluginsStopped;

    // --- Plugins whose MUTE setting is not the default -------------------------
    // Module FILE NAMES again, and a list of OVERRIDES rather than of settings:
    // an entry means "this plugin's 'mute audio while running' is the opposite
    // of what its capabilities imply".
    //
    // WHY OVERRIDES AND NOT VALUES. The default is derived from the plugin's
    // declared capabilities (muteDefaultForCaps: on for an I/Q decoder, off for
    // everything else), and it has to be, because a plugin the user has never
    // opened the row of must still behave correctly the first time they press
    // its preset. Storing the effective value instead would freeze whatever
    // default was in force the day the file was written, so a later build that
    // improved the rule would improve it for nobody who had ever run the old
    // one. Storing the difference means the rule stays live and only a
    // deliberate choice is remembered.
    //
    // The known cost, stated rather than discovered: if a plugin's declared
    // capabilities CHANGE across an update - an audio decoder that starts
    // consuming I/Q - an override recorded against the old default flips
    // meaning. That is the honest behaviour for "the opposite of the default"
    // and it is one click on a visible checkbox to correct, whereas the frozen
    // alternative is wrong forever and invisible.
    //
    // Sanitized on load exactly like the two lists above.
    std::vector<std::string> pluginMuteOverride;

    // --- Web server mode (P11) ------------------------------------------------
    // Browser access to the receiver. OFF by default, and the default binding
    // is this machine only, so enabling the feature cannot by itself expose
    // anything to a network.
    bool webEnabled = false;

    // The address the server binds. "127.0.0.1" (this machine only) and
    // "0.0.0.0" (every interface) are the two the UI offers; a specific
    // interface address is accepted for anyone who wants one.
    //
    // KEPT VERBATIM apart from the empty-string rule below, deliberately, for
    // the same reason pluginCatalogueUrl is: the value has exactly one
    // enforcement point — net/web_policy.hpp's evaluateBind — which refuses a
    // hostname, a malformed quad, an octet with a leading zero, and any
    // off-machine binding with no usable password. A second, weaker copy of
    // those rules here is how the two end up disagreeing.
    //
    // An EMPTY value resets to the default. This one exception matters: the
    // policy reads "" as "every interface", so an emptied field would silently
    // mean the OPPOSITE of the safe default, and a config file must not be
    // able to widen the binding by losing a value.
    std::string webBindAddress = "127.0.0.1";

    // Listening port. Sanitized on load (unlike the address) because it feeds a
    // numeric UI control, the same treatment volume and notchQ get.
    int webPort = 8073;

    // --- CAT control (rigctld-compatible) ---------------------------------
    // Lets logging and digital-mode software drive the receiver. OFF by
    // default, and loopback-only unless deliberately widened, for a blunter
    // reason than the web server's: this protocol has NO authentication of any
    // kind, so anything that can reach the port can retune the radio.
    bool catEnabled = false;
    bool catBindAll = false;
    // 4532 is the port rigctld uses, so clients need no configuring.
    int catPort = 4532;

    // Account name for the browser login. An empty value resets to the default.
    std::string webUsername = "admin";

    // Serialized PBKDF2 password record — see net/web_auth.hpp. Empty means no
    // password is set, which the policy allows only for a this-machine-only
    // binding. THE PLAINTEXT PASSWORD IS NEVER STORED, here or anywhere else;
    // there is deliberately no field it could live in. Kept verbatim, because
    // PasswordRecord::parse is its one enforcement point and an unreadable
    // record must refuse the bind rather than be quietly treated as "no
    // password".
    std::string webPasswordRecord;

    // --- Update check --------------------------------------------------------
    //
    // ON by default. It asks foxsdr.com once per launch whether a newer build
    // exists and what it fixed; nothing is downloaded or installed without a
    // click.
    //
    // The reason it defaults on is concrete rather than a preference: 0.55.0
    // fixed a fault that stopped every earlier build detecting any radio, and
    // of the 49 people who had taken one, 46 never returned to the site. There
    // was no way to tell them. A check that is off by default would have
    // reached exactly as many of them.
    //
    // It sends the running version and nothing else - no install id, no
    // identifier, no cookie kept. It is NOT the usage report (telemetry.hpp)
    // and the two share nothing.
    bool updateCheckEnabled = true;

    // --- Anonymous usage reporting (see PRIVACY.md) -------------------------
    //
    // ON by default; the user turns it off (product decision, 2026-08-18).
    //
    // NOTE FOR ANYONE CHANGING THIS BACK: PECR regulation 6 requires consent
    // before storing an identifier on someone's device for analytics, and a
    // default of true is not consent. The owner has accepted that risk
    // knowingly. If that position ever changes, this is the single line to
    // flip, and PRIVACY.md, README.md and the website all describe the
    // default in words that would need to change with it.
    bool telemetryEnabled = true;

    // Random 32-hex-character install id, created when reporting is switched
    // ON and DELETED when it is switched off, so a later opt-in cannot be
    // linked to an earlier one. Validated on load by
    // core::validInstallId — a hand-edited config that put something
    // meaningful here (a name, an email) is discarded rather than
    // transmitted.
    std::string telemetryInstallId;

    // Launches and unclean exits since installation. Counters only.
    std::uint64_t telemetryLaunches = 0;
    std::uint64_t telemetryCrashes = 0;

    // False while the application is running, true once it has shut down
    // cleanly. A start-up that finds this ALREADY false knows the previous
    // session ended in a crash — which is the only way to count crashes
    // without shipping a crash handler that uploads memory.
    bool telemetryCleanExit = true;

    // The finished session's report, JSON, waiting to be sent at the next
    // start-up. Held here rather than transmitted at exit because a network
    // call on the shutdown path can hang the application while the user is
    // trying to close it. Cleared once sent.
    std::string telemetryPending;

    // --- Local fault capture (see PRIVACY.md and docs/DIAGNOSTICS.md) -------
    //
    // ON by default, and that needs no consent argument the usage report does:
    // NOTHING IS UPLOADED. A crash report, a hang report and the rotating log
    // are written under %LOCALAPPDATA%\FoxSDR and stay there until the user
    // chooses to send one. Off means off - no directory, no file.
    bool diagnosticsEnabled = true;

    // A full minidump beside the text report. OFF by default and never
    // automatic: a minidump is process memory, which on this application can
    // include file paths and captured I/Q. Written LOCALLY when switched on,
    // and offered for manual sending; it is never uploaded by the application.
    bool diagnosticsMinidump = false;

    // --- Sending a captured report (see PRIVACY.md, core/crash_upload.hpp) --
    //
    // The client-side rate limit and duplicate memory, persisted because a
    // crash loop IS a sequence of runs: a limit that lived only in memory would
    // reset on every restart and limit nothing. None of this is transmitted -
    // it is the state that decides whether anything is.
    //
    // `crashUploadRecent` holds "<signature> <epoch seconds>" entries, bounded
    // and validated on load (core::decodePolicyState); `crashUploadBlockedUntil`
    // is set when the server answers 429 and is the honouring of it.
    std::vector<std::string> crashUploadRecent;
    std::uint64_t crashUploadWindowStart = 0;
    std::uint32_t crashUploadWindowCount = 0;
    std::uint64_t crashUploadBlockedUntil = 0;

    // Cap for the three plugin-name lists above (pluginTuneAllowed,
    // pluginsStopped, pluginMuteOverride); see the load-semantics note in the
    // header comment. One constant, because all three hold the same kind of
    // thing (module file names) and a second bound could only ever be a
    // second number to keep in step.
    static constexpr std::size_t kMaxTuneGrants = 256;

    // A pending report is a few hundred bytes; anything beyond this is a
    // corrupt or tampered config and is dropped rather than posted.
    static constexpr std::size_t kMaxPendingReportBytes = 8192;

    // Bounds for the map window geometry above. The minimum is a window that
    // can still show its own title bar and toolbar; below that the map is not
    // a map. The maximum is generous enough for any wall of monitors anyone
    // will plausibly own and small enough that a hand-edited or corrupt value
    // cannot ask the window manager for a rectangle it has to fight.
    static constexpr int kMapWindowMinPx = 320;
    static constexpr int kMapWindowMaxPx = 16384;

    // Cap for mapPages. Far more pages than plugins anyone installs, and small
    // enough that a hand-edited file cannot make the GUI iterate an unbounded
    // list every frame.
    static constexpr std::size_t kMaxMapPages = 64;
};

class ConfigStore {
public:
    // %APPDATA%/foxsdr/config.json on Windows (falls back to the
    // current directory if APPDATA is unset — an unset APPDATA means a
    // deliberately stripped environment, and "." at least stays writable);
    // $XDG_CONFIG_HOME or ~/.config equivalent elsewhere. The directory is
    // not created here — save() creates it when there is something to write.
    static std::string defaultPath();

    // Semantics documented in the header comment above. `out` is always
    // fully assigned: defaults first, then whatever the file legitimately
    // overrides — so a false return still leaves a usable config.
    static bool load(const std::string& path, AppConfig& out, std::string& error);

    // Atomic write (temp file + rename over target, see header comment).
    // On failure returns false with `error` set and the previous target
    // content — if any — intact; the temp file is cleaned up.
    static bool save(const std::string& path, const AppConfig& cfg, std::string& error);
};

}  // namespace cascade::core
