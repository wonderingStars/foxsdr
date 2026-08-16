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

    // Whether the "Get plugins" browser was left open. Purely cosmetic
    // restore of where the user was — it does NOT cause a fetch on startup.
    // Nothing in this product touches the catalogue origin until the user
    // presses Browse; that promise is what the browser's privacy note makes,
    // and a config field that could reinstate a network call behind the
    // user's back would break it.
    bool pluginBrowserOpen = false;

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

    // Cap for the list above; see the load-semantics note in the header comment.
    static constexpr std::size_t kMaxTuneGrants = 256;
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
