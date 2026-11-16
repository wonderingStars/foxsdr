// s5b_logic.cpp - see s5b_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s5b_logic.hpp"

#include <cstdlib>

#include "core/i18n.hpp"

namespace foxsdr::slints5b {

namespace {
// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
using foxsdr::slintlanel::numberText;
using foxsdr::slintlanel::parseNumber;
using foxsdr::slintlanel::trim;
}  // namespace

// ============================================================ analogue IF bandwidth ============================================================
bool parseIfBandwidthKHz(const std::string& text, long long& hz, std::string& err) {
    if (trim(text).empty()) { hz = 0; return true; }
    double khz = 0.0;
    if (!parseNumber(text, 0.0, 8000.0, khz, err)) { return false; }
    if (khz != 0.0 && khz < 300.0) { err = tr("allowed:") + " 300 - 8000 (0 = automatic)"; return false; }
    hz = static_cast<long long>(khz * 1000.0 + 0.5);
    return true;
}

std::string ifBandwidthWords(const std::string& fact) {
    if (fact.empty() || fact.find_first_not_of("0123456789") != std::string::npos) { return std::string(); }
    const long long hz = std::atoll(fact.c_str());
    if (hz == 0) { return tr("Automatic (from the sample rate)"); }
    return numberText(static_cast<double>(hz) / 1000.0) + " kHz";
}

}  // namespace foxsdr::slints5b
