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
//
// Save semantics: ATOMIC. The JSON is written to a temp file in the target's
// directory, then renamed over the target, so a crash, full disk, or locked
// target leaves either the complete old config or the complete new one on
// disk — never a truncated hybrid. Parent directories are created on demand.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace cascade::core {

struct AppConfig {
    int schemaVersion = 1;
    std::string sourceKind = "siggen";      // "siggen" | "file" | "soapy"
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
};

class ConfigStore {
public:
    // %APPDATA%/sdr-minus-plus/config.json on Windows (falls back to the
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
