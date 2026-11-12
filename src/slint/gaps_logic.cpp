// gaps_logic.cpp - see gaps_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gaps_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/app_commands.hpp"
#include "host_logic.hpp"

namespace foxsdr::slintgaps {

std::string dottedFrequencyText(double hz, int style) {
    if (!(hz >= 0.0)) { hz = 0.0; }
    const unsigned long long total = static_cast<unsigned long long>(std::llround(std::min(hz, 9.9e10)));
    const unsigned long long mhz = total / 1000000ull, khz = (total / 1000ull) % 1000ull, h = total % 1000ull;
    char tail[16];
    std::snprintf(tail, sizeof(tail), ".%03llu.%03llu", khz, h);
    if (style == 1 || mhz < 1000ull) {
        // MHz.kHz.Hz; below 1000 MHz the two styles are the same text (the thousands of the MHz figure start at 1000)
        return std::to_string(mhz) + tail;
    }
    // thousands: the MHz figure's own thousands are a group of their own (1090 MHz = 1.090)
    char g[8];
    std::snprintf(g, sizeof(g), ".%03llu", mhz % 1000ull);
    return std::to_string(mhz / 1000ull) + g + tail;
}

bool parseTypedFrequencyHz(const std::string& text, double& hz) {
    const auto dots = std::count(text.begin(), text.end(), '.');
    if (dots < 2) { return foxsdr::slinthost::parseFrequencyHz(text, hz); }
    std::string digits, unit;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') { digits += ch; }
        else if (ch == '.' || ch == ' ' || ch == '\t' || ch == '_' || ch == '\'') { continue; }
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) { unit += static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        else { return false; }
    }
    if (digits.empty() || digits.size() > 12) { return false; }
    const double value = std::strtod(digits.c_str(), nullptr);
    double scale = 0.0;
    if (unit.empty() || unit == "hz") { scale = 1.0; }
    else if (unit == "khz" || unit == "k") { scale = 1e3; }
    else if (unit == "mhz" || unit == "m") { scale = 1e6; }
    else if (unit == "ghz") { scale = 1e9; }
    else { return false; }
    hz = value * scale;
    return hz > 0.0 && hz < 1e11;
}

bool isTuningCommand(const FoxCommand& c) {
    switch (c.op) {
        case FOXAPI_OP_SET_CENTRE:
        case FOXAPI_OP_SET_FREQUENCY:
        case FOXAPI_OP_SET_VFO_OFFSET:
        case FOXAPI_OP_STEP_TUNE:
        case FOXAPI_OP_BOOKMARK_TUNE:
        case FOXAPI_OP_PLUGIN_PRESET:
        case FOXAPP_OP_VFO_TO_ABSOLUTE:        // click-to-tune on the spectrum and the waterfall
        case FOXAPP_OP_SET_VFO_OFFSET_FREE:    // the VFO slider
        case FOXAPP_OP_USER_PRESET_APPLY: return true;   // a user preset applies a frequency
        case FOXAPP_OP_SCANNER_LIST: return c.ival[0] == 2;   // 2 = start a list scan (0 and 1 only choose the mode)
        case FOXAPP_OP_AIRBAND: return c.ival[0] == 1 || c.ival[0] == 2 || c.ival[0] == 6;   // listen, next block, tune one
        case FOXAPI_OP_SCANNER_RUN: return c.ival[0] != 0;
        default: return false;
    }
}

FrequencyLock& FrequencyLock::instance() {
    static FrequencyLock lock;
    return lock;
}

int FrequencyLock::radioOf(const FoxCommand& c, std::uint32_t radioOr0) const {
    if (c.target != 0u && c.target != FOXAPI_TARGET_ALL_RADIOS) { return static_cast<int>(c.target); }
    if (radioOr0 != 0u) { return static_cast<int>(radioOr0); }
    return selected();
}

bool FrequencyLock::refuses(const FoxCommand& c, std::uint32_t radioOr0) {
    if (mask() == 0u || !isTuningCommand(c)) { return false; }
    const int r = radioOf(c, radioOr0);
    if (!locked(r)) { return false; }
    lastRefused_.store(r, std::memory_order_relaxed);
    refusals_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

namespace {
const FoxEngineApi* g_real = nullptr;

int32_t FOXAPI_CALL lockedSubmit(FoxSession* session, const FoxCommand* commands, uint32_t count, FoxSubmitResult* results) {
    FrequencyLock& lock = FrequencyLock::instance();
    if (commands == nullptr || count == 0u || lock.mask() == 0u) { return g_real->submit(session, commands, count, results); }
    std::vector<FoxCommand> pass;
    std::vector<std::uint32_t> passIndex;
    pass.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (lock.refuses(commands[i])) {
            if (results != nullptr) { results[i].structSize = sizeof(FoxSubmitResult); results[i].status = FOXAPI_DENIED; results[i].ticket = 0; }
            continue;
        }
        pass.push_back(commands[i]);
        passIndex.push_back(i);
    }
    if (pass.size() == count) { return g_real->submit(session, commands, count, results); }
    if (pass.empty()) { return 0; }
    std::vector<FoxSubmitResult> sub(pass.size());
    for (FoxSubmitResult& s : sub) { s.structSize = sizeof(FoxSubmitResult); }
    const int32_t rc = g_real->submit(session, pass.data(), static_cast<uint32_t>(pass.size()), results != nullptr ? sub.data() : nullptr);
    if (rc >= 0 && results != nullptr) { for (std::size_t k = 0; k < pass.size(); ++k) { results[passIndex[k]] = sub[k]; } }
    return rc;
}

int32_t FOXAPI_CALL lockedSubmitText(FoxSession* session, const FoxCommand* command, const char* text, uint32_t len, FoxSubmitResult* result) {
    if (command != nullptr && FrequencyLock::instance().refuses(*command)) {
        if (result != nullptr) { result->structSize = sizeof(FoxSubmitResult); result->status = FOXAPI_DENIED; result->ticket = 0; }
        return 0;
    }
    return g_real->submit_text(session, command, text, len, result);
}
}  // namespace

const FoxEngineApi* lockedApi(const FoxEngineApi* real) {
    static FoxEngineApi copy;
    if (real == nullptr) { return nullptr; }
    g_real = real;
    copy = *real;
    if (FOXAPI_HAS(real, submit)) { copy.submit = &lockedSubmit; }
    if (FOXAPI_HAS(real, submit_text)) { copy.submit_text = &lockedSubmitText; }
    return &copy;
}

DropKind classifyDrop(const std::string& path) {
    std::string p = path;
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') { p = p.substr(1, p.size() - 2); }
    const std::size_t dot = p.find_last_of('.');
    const std::size_t slash = p.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) { return DropKind::Unknown; }
    std::string ext = p.substr(dot + 1);
    for (char& ch : ext) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
    if (ext == "wav") { return DropKind::IqRecording; }
    if (ext == "xml" || ext == "csv") { return DropKind::FrequencyList; }
    return DropKind::Unknown;
}

const char* manualUrl() { return "https://github.com/wonderingStars/foxsdr/blob/master/docs/MANUAL.md"; }

}  // namespace foxsdr::slintgaps
