// contest_macros.hpp - contest text macros (hamops lane): templates such as "{MYCALL} {NR:3} {X:ZONE}" expand to PLAIN TEXT from a contest
// session (own call, the other station's call, the next serial number, the fixed exchange values). The model YIELDS TEXT ONLY.
//
// NOTHING IS KEYED OR SENT: this header includes no engine, audio, network or transmit header and has no function that sends, keys or plays
// anything; whoever wires it (a screen lane) decides what a user action does with the text. tests/test_contest_macros.cpp scans this file for
// every transmit-path word and fails if one appears.
//
// Not the same thing as the engine macro model on branch feat/core-formats (named command sequences with delays): that sends commands, this
// only expands text.
//
// Cut numbers (0 -> T, 9 -> N ...) are NOT built: no opened source states the mapping.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cabrillo.hpp"
#include "qso_log.hpp"

namespace cascade::core::ham {

inline constexpr std::size_t kMacroTemplateMax = 256;
inline constexpr std::size_t kMacroOutputMax = 512;
inline constexpr int kMacroSlots = 12;   // F1..F12

struct MacroResult {
    bool ok = false;
    std::string text;
    std::string error;   // a sentence; never contains the template's data
};

class ContestSession {
public:
    ContestSession() = default;
    // `contestId` must be a table contest (cabrillo.hpp); false and a sentence otherwise.
    bool begin(const std::string& contestId, const std::string& myCall, std::string& error) {
        const ContestDef* d = findCabrilloContest(contestId);
        if (d == nullptr) { error = "that contest has no exchange table"; return false; }
        std::string c;
        if (!normaliseCall(myCall, c)) { error = "own callsign is empty or has characters other than A-Z, 0-9 and /"; return false; }
        def_ = d;
        myCall_ = c;
        nextSerial_ = 1;
        fixed_.clear();
        slots_.assign(kMacroSlots, std::string());
        rst_ = (d->modeCategory == "CW") ? "599" : "59";
        return true;
    }
    bool active() const { return def_ != nullptr; }
    const ContestDef* contest() const { return def_; }
    const std::string& myCall() const { return myCall_; }
    int nextSerial() const { return nextSerial_; }
    void setNextSerial(int n) { if (n >= 1 && n <= 99999) { nextSerial_ = n; } }
    // A fixed exchange value (ZONE, SECT, PREC, CHECK, NAME ...): letters and digits only, at most 12.
    bool setValue(const std::string& key, const std::string& value) {
        if (key.empty() || key.size() > 12 || value.size() > 12) { return false; }
        for (char c : key) { if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) { return false; } }
        for (char c : value) { if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) { return false; } }
        fixed_[key] = upperAscii(value);
        return true;
    }
    void setRst(const std::string& rst) { if (rst.size() >= 2 && rst.size() <= 3) { rst_ = rst; } }
    bool setSlot(int slot, const std::string& tmpl) {
        if (slot < 1 || slot > kMacroSlots || tmpl.size() > kMacroTemplateMax) { return false; }
        slots_[static_cast<std::size_t>(slot - 1)] = tmpl;
        return true;
    }
    const std::string& slot(int slot) const {
        static const std::string empty;
        if (slot < 1 || slot > kMacroSlots || slots_.empty()) { return empty; }
        return slots_[static_cast<std::size_t>(slot - 1)];
    }

    // Tokens: {MYCALL} {CALL} {NR} {NR:w} {RST} {X:KEY}; "{{" and "}}" are literal braces. Unknown token, unterminated brace, a stray "}" or a result
    // over kMacroOutputMax is an error and yields NO text. `theirCall` is whatever is typed in the call box (upper-cased; may be empty).
    MacroResult expand(const std::string& tmpl, const std::string& theirCall) const {
        MacroResult r;
        if (!active()) { r.error = "no contest session is running"; return r; }
        if (tmpl.size() > kMacroTemplateMax) { r.error = "the template is too long"; return r; }
        std::string out;
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            const char c = tmpl[i];
            if (c == '}') {
                if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') { out.push_back('}'); ++i; continue; }
                r.error = "a closing brace has no opening brace";
                return r;
            }
            if (c != '{') { out.push_back(c); continue; }
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { out.push_back('{'); ++i; continue; }
            const std::size_t end = tmpl.find('}', i + 1);
            if (end == std::string::npos) { r.error = "a brace is not closed"; return r; }
            const std::string tok = tmpl.substr(i + 1, end - i - 1);
            std::string val;
            if (!tokenValue(tok, theirCall, val)) { r.error = "unknown macro token"; return r; }
            out += val;
            i = end;
            if (out.size() > kMacroOutputMax) { r.error = "the expansion is too long"; return r; }
        }
        if (out.size() > kMacroOutputMax) { r.error = "the expansion is too long"; return r; }
        r.ok = true;
        r.text = std::move(out);
        return r;
    }
    MacroResult expandSlot(int slotNo, const std::string& theirCall) const { return expand(slot(slotNo), theirCall); }

    // The exchange tokens this station SENDS for the next QSO, in the contest's field order (RST, NR and the fixed values). Empty vector when a
    // fixed value the contest needs has not been set (so a QSO is never logged with an invented exchange).
    std::vector<std::string> sentTokens() const {
        std::vector<std::string> v;
        if (!active()) { return v; }
        for (const ExchField& f : def_->exch) {
            std::string val;
            if (!fieldValue(f.key, val)) { return std::vector<std::string>(); }
            v.push_back(val);
        }
        return v;
    }
    // Call after the QSO was logged: the serial number moves on. Never before: a re-sent exchange keeps its number.
    void logged() { if (nextSerial_ < 99999) { ++nextSerial_; } }

private:
    bool fieldValue(const std::string& key, std::string& out) const {
        if (key == "RST") { out = rst_; return true; }
        if (key == "NR") { out = std::to_string(nextSerial_); return true; }
        auto it = fixed_.find(key);
        if (it == fixed_.end()) { return false; }
        out = it->second;
        return true;
    }
    bool tokenValue(const std::string& tok, const std::string& theirCall, std::string& out) const {
        if (tok == "MYCALL") { out = myCall_; return true; }
        if (tok == "CALL") { out = upperAscii(theirCall); return true; }
        if (tok == "RST") { out = rst_; return true; }
        if (tok == "NR") { out = std::to_string(nextSerial_); return true; }
        if (tok.rfind("NR:", 0) == 0) {
            const std::string w = tok.substr(3);
            if (w.size() != 1 || w[0] < '1' || w[0] > '5') { return false; }
            std::string s = std::to_string(nextSerial_);
            const std::size_t width = static_cast<std::size_t>(w[0] - '0');
            if (s.size() < width) { s = std::string(width - s.size(), '0') + s; }
            out = s;
            return true;
        }
        if (tok.rfind("X:", 0) == 0) {
            auto it = fixed_.find(tok.substr(2));
            if (it == fixed_.end()) { return false; }
            out = it->second;
            return true;
        }
        return false;
    }

    const ContestDef* def_ = nullptr;
    std::string myCall_;
    int nextSerial_ = 1;
    std::string rst_ = "599";
    std::map<std::string, std::string> fixed_;
    std::vector<std::string> slots_;
};

}  // namespace cascade::core::ham
