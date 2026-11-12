// access_speech.hpp - spoken state: what the receiver says out loud about its
// frequency, mode and signal level, when, and in which words. Pure logic plus a
// Speaker interface; the Windows speech engine lives in access_speech_sapi.*.
//
// THREE PARTS
//   1. Phrases (PhraseTable, phrase*()): "7.074 megahertz", "upper sideband",
//      "S 7", "S 9 plus 20 decibels", "minus 82 decibel milliwatts". Numbers go
//      through access_number_format.hpp with an explicit NumberSymbols, so a
//      German voice is handed "7,074 Megahertz". The words are an English table
//      (PhraseTable) listed in the feature record for translation; nothing here
//      is wired to the catalogue.
//   2. Policy (AnnouncePolicy) and Announcer: ON DEMAND (a key press asks, the
//      whole state is spoken, intervals ignored) and ON CHANGE (the state is
//      polled; only what changed is spoken, rate-limited).
//   3. Speaker: the interface, a RecordingSpeaker test double here, and a SAPI
//      implementation (access_speech_sapi.hpp) whose output is ALWAYS a memory
//      stream: it refuses to speak at all unless that stream is attached, so
//      nothing can reach the speakers.
//
// ON-CHANGE RULES (each is a test)
//   * The first state seen is the baseline; nothing is spoken for it
//     (announceOnStart = false). A baseline is also set by every spoken
//     utterance and by onDemand().
//   * FREQUENCY: a move of at least minFreqChangeHz from the last SPOKEN value
//     is pending; it is spoken only once the dial has stopped for settleMs, so
//     turning the knob through 50 channels produces ONE announcement of the
//     final channel. Speaking a frequency also speaks the mode (context).
//   * MODE: a mode change is spoken on its own, unless a frequency change is
//     pending, in which case they go together after the settle.
//   * LEVEL: spoken when it differs from the last spoken level by at least
//     levelStepDb (6 dB = one S-unit by default); a change of squelch state
//     speaks "signal" / "no signal" (the level is not read while the squelch
//     is closed).
//   * Nothing automatic is spoken within minIntervalMs of the previous
//     utterance; a pending change waits and is then spoken with the CURRENT
//     values, never stale ones.
//   * Unknown (NaN) frequency or level, an empty mode: that part is skipped.
//
// S-UNITS. IARU Region 1 Technical Recommendation R.1 (1981): S9 = -73 dBm on
// HF (below 30 MHz) and -93 dBm on VHF/UHF (above 30 MHz), 6 dB per S-unit
// (read on 2026-11-11 in the Wikipedia article "S meter", which quotes it; the
// IARU document itself was not opened: SECONDARY SOURCE). Above S9 the spoken
// form is "S 9 plus N decibels" with N to the nearest 10. A level that is not
// calibrated (no dBm) is spoken in decibels, "relative", never as S-units.
//
// NO ONE HAS LISTENED to any of this and no screen reader (NVDA, JAWS) was
// installed or run; see the feature record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/access_number_format.hpp"

namespace cascade::core::access {

// ---- the words -----------------------------------------------------------------

struct PhraseTable {
    // {n} is replaced by the formatted number.
    std::string hertz = "{n} hertz";
    std::string kilohertz = "{n} kilohertz";
    std::string megahertz = "{n} megahertz";
    std::string gigahertz = "{n} gigahertz";
    std::string sUnit = "S {n}";
    std::string sNinePlus = "S 9 plus {n} decibels";
    std::string decibels = "{n} decibels";
    std::string decibelMilliwatts = "{n} decibel milliwatts";
    std::string relativeDecibels = "level {n} decibels";   // an uncalibrated level
    std::string minus = "minus";
    std::string signal = "signal";
    std::string noSignal = "no signal";
    std::string joiner = ", ";
    // Mode words as spoken, keyed by the upper-case mode name. A name not in the
    // map is spoken as written.
    std::map<std::string, std::string> modes = {
        {"NFM", "narrow F M"}, {"WFM", "wide F M"}, {"AM", "A M"}, {"DSB", "double sideband"},
        {"USB", "upper sideband"}, {"LSB", "lower sideband"}, {"CW", "C W"}, {"RAW", "raw"},
    };
};

inline std::string substituteN(const std::string& tmpl, const std::string& n) {
    std::string out;
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl.compare(i, 3, "{n}") == 0) { out += n; i += 3; } else { out += tmpl[i++]; }
    }
    return out;
}

namespace speechdetail {

// Trim trailing zeros and a trailing decimal separator from a formatted number
// (works on the formatted text because the decimal symbol is known).
inline std::string trimmed(double v, int maxDecimals, const NumberSymbols& sym) {
    std::string out = formatNumber(v, maxDecimals, sym, DigitPolicy::Latin, false);
    const std::size_t at = sym.decimal.empty() ? std::string::npos : out.rfind(sym.decimal);
    if (at == std::string::npos) { return out; }
    while (out.size() > at + sym.decimal.size() && out.back() == '0') { out.pop_back(); }
    if (out.size() == at + sym.decimal.size()) { out.erase(at); }   // nothing left after the separator
    return out;
}

}  // namespace speechdetail

// "7.074 megahertz". Unknown (NaN, infinite, negative) gives "".
inline std::string frequencyPhrase(double hz, const NumberSymbols& sym, const PhraseTable& t = PhraseTable()) {
    if (!std::isfinite(hz) || hz < 0.0) { return std::string(); }
    if (hz >= 1e9) { return substituteN(t.gigahertz, speechdetail::trimmed(hz / 1e9, 6, sym)); }
    if (hz >= 1e6) { return substituteN(t.megahertz, speechdetail::trimmed(hz / 1e6, 6, sym)); }
    if (hz >= 1e3) { return substituteN(t.kilohertz, speechdetail::trimmed(hz / 1e3, 3, sym)); }
    return substituteN(t.hertz, speechdetail::trimmed(hz, 0, sym));
}

inline std::string modePhrase(const std::string& mode, const PhraseTable& t = PhraseTable()) {
    if (mode.empty()) { return std::string(); }
    std::string up = mode;
    for (char& c : up) { if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); } }
    const auto it = t.modes.find(up);
    return it == t.modes.end() ? mode : it->second;
}

enum class LevelStyle : std::uint8_t { SUnits = 0, Decibels };

// The S-unit reading for a calibrated level. Reference by frequency: S9 is
// -73 dBm below 30 MHz and -93 dBm from 30 MHz up (frequency NaN = HF).
struct SReading {
    int s = 0;           // 0..9
    int overS9Db = 0;    // 0, 10, 20 ... when s == 9 and above it
};
inline SReading sUnitsFromDbm(double dbm, double freqHz) {
    const double s9 = (std::isfinite(freqHz) && freqHz >= 30e6) ? -93.0 : -73.0;
    SReading r;
    const double over = dbm - s9;                  // dB above S9
    if (over > 0.0) {
        r.s = 9;
        r.overS9Db = static_cast<int>(std::lround(over / 10.0)) * 10;
        return r;
    }
    const double units = 9.0 + over / 6.0;         // 6 dB per S-unit
    r.s = static_cast<int>(std::lround(units));
    if (r.s < 0) { r.s = 0; }
    if (r.s > 9) { r.s = 9; }
    return r;
}

inline std::string signedWords(double v, int decimals, const NumberSymbols& sym, const PhraseTable& t,
                               const std::string& tmpl) {
    const std::string num = formatNumber(std::fabs(v), decimals, sym, DigitPolicy::Latin, false);
    const bool neg = v < 0.0 && formatNumber(v, decimals, sym) != formatNumber(0.0, decimals, sym);
    return substituteN(tmpl, neg ? t.minus + " " + num : num);
}

// One of dbm / relDb is known. dbm calibrated (a real dBm figure): S-units or
// decibel milliwatts per `style`. relDb (the meter's own scale, not calibrated):
// always decibels, said to be a level, never S-units.
inline std::string levelPhrase(double dbm, double relDb, double freqHz, LevelStyle style,
                               const NumberSymbols& sym, const PhraseTable& t = PhraseTable()) {
    if (std::isfinite(dbm)) {
        if (style == LevelStyle::SUnits) {
            const SReading r = sUnitsFromDbm(dbm, freqHz);
            if (r.s == 9 && r.overS9Db > 0) { return substituteN(t.sNinePlus, std::to_string(r.overS9Db)); }
            return substituteN(t.sUnit, std::to_string(r.s));
        }
        return signedWords(dbm, 0, sym, t, t.decibelMilliwatts);
    }
    if (std::isfinite(relDb)) { return signedWords(relDb, 0, sym, t, t.relativeDecibels); }
    return std::string();
}

// ---- what is spoken about -----------------------------------------------------------

struct SpokenState {
    double freqHz = std::numeric_limits<double>::quiet_NaN();
    std::string mode;
    double levelDbm = std::numeric_limits<double>::quiet_NaN();   // calibrated, if known
    double levelRelDb = std::numeric_limits<double>::quiet_NaN(); // the meter's own scale
    bool hasSquelch = false;
    bool squelchOpen = true;
};

struct AnnouncePolicy {
    bool onChange = false;            // automatic announcements (default OFF: on demand only)
    bool announceOnStart = false;
    bool speakFrequency = true;
    bool speakMode = true;
    bool speakLevel = true;
    LevelStyle levelStyle = LevelStyle::SUnits;
    double minIntervalMs = 3000.0;    // between any two automatic utterances
    double settleMs = 800.0;          // the dial must rest this long before a frequency is spoken
    double minFreqChangeHz = 100.0;   // smaller moves are not announced
    double levelStepDb = 6.0;         // one S-unit
    bool interrupt = true;            // a new utterance cuts the previous one off
};

struct Utterance {
    std::string text;
    bool interrupt = true;
};

class Announcer {
public:
    Announcer(AnnouncePolicy policy, NumberSymbols symbols, PhraseTable phrases = PhraseTable())
        : p_(std::move(policy)), sym_(std::move(symbols)), t_(std::move(phrases)) {}

    void setPolicy(const AnnouncePolicy& p) { p_ = p; }
    const AnnouncePolicy& policy() const { return p_; }
    void setSymbols(const NumberSymbols& s) { sym_ = s; }

    // The whole state, now, on request. Resets the baseline so the next
    // on-change poll does not repeat what was just said.
    Utterance onDemand(const SpokenState& s, double nowMs) {
        std::vector<std::string> parts;
        if (p_.speakFrequency) { addIf(parts, frequencyPhrase(s.freqHz, sym_, t_)); }
        if (p_.speakMode) { addIf(parts, modePhrase(s.mode, t_)); }
        if (p_.speakLevel) { addIf(parts, levelPart(s)); }
        baseline(s, nowMs);
        Utterance u;
        u.interrupt = p_.interrupt;
        u.text = join(parts);
        if (u.text.empty()) { u.text = t_.noSignal; }   // nothing known at all: say so rather than be silent
        return u;
    }

    // Poll with the current state. Returns an utterance when one is due.
    std::optional<Utterance> update(const SpokenState& s, double nowMs) {
        if (!p_.onChange) { seen_ = s; seenValid_ = true; return std::nullopt; }
        if (!haveBaseline_) {
            baseline(s, nowMs);
            lastUtteranceAt_ = -1e18;
            if (!p_.announceOnStart) { return std::nullopt; }
            Utterance u = onDemand(s, nowMs);
            return u;
        }
        // Track the dial's movement for the settle rule.
        if (!seenValid_ || differs(seen_.freqHz, s.freqHz, 1e-3)) { freqMovedAt_ = nowMs; }
        seen_ = s; seenValid_ = true;

        bool freqPending = false;
        if (p_.speakFrequency && std::isfinite(s.freqHz)) {
            freqPending = !std::isfinite(spoken_.freqHz) || std::fabs(s.freqHz - spoken_.freqHz) >= p_.minFreqChangeHz;
        }
        const bool modePending = p_.speakMode && !s.mode.empty() && s.mode != spoken_.mode;
        bool levelPending = false;
        if (p_.speakLevel) {
            if (s.hasSquelch && s.squelchOpen != spoken_.squelchOpen) { levelPending = true; }
            const double now = levelNumber(s), then = levelNumber(spoken_);
            if (!(s.hasSquelch && !s.squelchOpen) && std::isfinite(now) &&
                (!std::isfinite(then) || std::fabs(now - then) >= p_.levelStepDb)) { levelPending = true; }
        }
        if (!freqPending && !modePending && !levelPending) { return std::nullopt; }
        if (freqPending && nowMs - freqMovedAt_ < p_.settleMs) { return std::nullopt; }      // dial still moving
        if (nowMs - lastUtteranceAt_ < p_.minIntervalMs) { return std::nullopt; }            // too soon; stays pending

        std::vector<std::string> parts;
        if (freqPending) {
            addIf(parts, frequencyPhrase(s.freqHz, sym_, t_));
            if (p_.speakMode) { addIf(parts, modePhrase(s.mode, t_)); }
        } else if (modePending) {
            addIf(parts, modePhrase(s.mode, t_));
        }
        if (levelPending) { addIf(parts, levelPart(s)); }
        Utterance u;
        u.interrupt = p_.interrupt;
        u.text = join(parts);
        if (u.text.empty()) { return std::nullopt; }
        // Record exactly what was said, part by part.
        if (freqPending) { spoken_.freqHz = s.freqHz; }
        if (freqPending || modePending) { spoken_.mode = s.mode; }
        if (levelPending) { spoken_.levelDbm = s.levelDbm; spoken_.levelRelDb = s.levelRelDb; spoken_.squelchOpen = s.squelchOpen; spoken_.hasSquelch = s.hasSquelch; }
        lastUtteranceAt_ = nowMs;
        return u;
    }

private:
    static bool differs(double a, double b, double eps) {
        if (std::isnan(a) != std::isnan(b)) { return true; }
        if (std::isnan(a)) { return false; }
        return std::fabs(a - b) > eps;
    }
    static void addIf(std::vector<std::string>& v, const std::string& s) { if (!s.empty()) { v.push_back(s); } }
    std::string join(const std::vector<std::string>& v) const {
        std::string out;
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) { out += t_.joiner; } out += v[i]; }
        return out;
    }
    static double levelNumber(const SpokenState& s) { return std::isfinite(s.levelDbm) ? s.levelDbm : s.levelRelDb; }
    std::string levelPart(const SpokenState& s) const {
        if (s.hasSquelch && !s.squelchOpen) { return t_.noSignal; }
        const std::string l = levelPhrase(s.levelDbm, s.levelRelDb, s.freqHz, p_.levelStyle, sym_, t_);
        if (l.empty()) { return std::string(); }
        return l;
    }
    void baseline(const SpokenState& s, double nowMs) {
        spoken_ = s;
        seen_ = s; seenValid_ = true;
        freqMovedAt_ = nowMs;
        lastUtteranceAt_ = nowMs;
        haveBaseline_ = true;
    }

    AnnouncePolicy p_;
    NumberSymbols sym_;
    PhraseTable t_;
    bool haveBaseline_ = false;
    SpokenState spoken_;
    SpokenState seen_;
    bool seenValid_ = false;
    double freqMovedAt_ = 0.0;
    double lastUtteranceAt_ = -1e18;
};

// ---- speakers --------------------------------------------------------------------

class Speaker {
public:
    virtual ~Speaker() = default;
    // false when nothing could be spoken (no engine, no safe output). Never
    // blocks longer than the engine needs to render the text.
    virtual bool speak(const std::string& utf8, bool interrupt) = 0;
    virtual void stop() = 0;
    virtual std::string name() const = 0;
};

// The test double: records what it was asked to say.
class RecordingSpeaker : public Speaker {
public:
    struct Entry { std::string text; bool interrupt; };
    bool speak(const std::string& utf8, bool interrupt) override {
        if (!available) { return false; }
        spoken.push_back({utf8, interrupt});
        return true;
    }
    void stop() override { ++stops; }
    std::string name() const override { return "recording"; }
    std::vector<Entry> spoken;
    int stops = 0;
    bool available = true;
};

// Drive an announcer into a speaker: the single place an utterance is spoken.
inline bool speakIfDue(Announcer& a, Speaker& sp, const SpokenState& s, double nowMs) {
    const std::optional<Utterance> u = a.update(s, nowMs);
    if (!u) { return false; }
    return sp.speak(u->text, u->interrupt);
}
inline bool speakNow(Announcer& a, Speaker& sp, const SpokenState& s, double nowMs) {
    const Utterance u = a.onDemand(s, nowMs);
    return sp.speak(u.text, u.interrupt);
}

}  // namespace cascade::core::access
