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
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_SCANNER_PARAMS_HPP
