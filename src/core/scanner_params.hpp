// scanner_params.hpp - the scanner's parameters, as data (engine extraction stage 4, the fence).
//
// Moved out of core/scanner.hpp (0.99.84 merge: with master's `list`, the airband monitor's scan list): the window holds a DRAFT of them (the scanner form, committed as one
// FOXAPI_OP_SCANNER_CONFIG) and the scanner is the engine's, which the interface may not include. Scanner::Params names it back.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SCANNER_PARAMS_HPP
#define CASCADE_CORE_SCANNER_PARAMS_HPP

#include <vector>

namespace cascade::core {

struct ScannerParams {
    double startHz = 88e6;
    double stopHz = 108e6;
    double stepHz = 100e3;
    double dwellMs = 50;
    double holdMs = 2000;
    double resumeMs = 500;
    double listenMs = 0;  // longest stay on one signal; 0 = until quiet
    // Non-empty: scan these, in this order, instead of the range (core/scanner.hpp, "List mode").
    std::vector<double> list;

    // --- screen lane C additions (2026-11-11): every field defaults to "off", so an engine that does not know them scans exactly as before ---
    // Permanent lockouts (C3): frequencies the scan never lands on. A lattice point or list entry within lockoutEpsHz of one is passed over.
    std::vector<double> lockedHz;
    // Priority channels (C2): visited every priorityIntervalMs of scanning time (0 = never), one per visit in turn, for dwellMs each. A signal on
    // one holds the scan there like any other; with none, the scan goes back to the frequency it left.
    std::vector<double> priorityHz;
    double priorityIntervalMs = 0;
    // Per-entry thresholds (C1): thresholdDb[k] belongs to list[k] (NaN or a missing entry = the squelch decides, as before). With one, and a level
    // supplied to tick(), the signal counts as present when the level reaches it, whatever the squelch says.
    std::vector<double> thresholdDb;
    // Mute while scanning (C5): the scanner says muted() while it steps and not while it listens.
    bool muteWhileScanning = false;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_SCANNER_PARAMS_HPP
