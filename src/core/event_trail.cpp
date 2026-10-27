// event_trail.cpp - see event_trail.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/event_trail.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

namespace cascade::core::trail {

namespace {

// The qualifier tables. Each is held to its source of truth by tests/test_event_trail.cpp: the
// drivers to health::vocabulary()'s driver list, the steps to core/plugin_reload_phases.hpp,
// the modes to the application's kModeNames.
constexpr const char* kDrivers[] = {"rtlsdr",   "hackrf", "airspy", "airspyhf",  "sdrplay",
                                    "mirisdr",  "rx888",  "pluto",  "aor",       "hydrasdr",
                                    "rtltcp",   "soapy",  "soundcard", "other"};
constexpr const char* kModes[] = {"nfm", "wfm", "am", "dsb", "usb", "cw", "lsb", "raw"};
// The reload's steps in the order of core/plugin_reload_phases.hpp's names, hyphenated for the
// wire ("panels and map" has spaces, which the wire's grammar does not).
constexpr const char* kSteps[] = {"check", "decoders", "patch", "panels-and-map",
                                  "unload", "inventory", "load", "restart"};
// The PhaseClock's spelling of each, in the same order.
constexpr const char* kStepNames[] = {"check", "decoders", "patch", "panels and map",
                                      "unload", "inventory", "load", "restart"};

template <class T, std::size_t N>
constexpr unsigned countOf(const T (&)[N]) {
    return static_cast<unsigned>(N);
}

constexpr const char* kEventWords[kEventCount + 1] = {
    "", "src-open", "src-close", "mode", "rescan", "rec-start", "rec-stop", "plug-fit", "plug-unfit"};

// One slot is one 64-bit word: the moment (steady milliseconds) in the top 48 bits, the
// qualifier index in bits 8..15 and the event in bits 0..7. An event is never 0, so a word of 0
// is an empty slot.
std::atomic<std::uint64_t> g_slots[kEntries];
std::atomic<std::uint64_t> g_head{0};

std::uint64_t steadyMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char* const* tableOf(Event e, unsigned& n) noexcept {
    switch (e) {
        case Event::SrcOpen:
        case Event::SrcClose: n = countOf(kDrivers); return kDrivers;
        case Event::Mode: n = countOf(kModes); return kModes;
        case Event::Rescan: n = countOf(kSteps); return kSteps;
        default: n = 0; return nullptr;
    }
}

// Appends text to buf at `pos` while room is left for the NUL; false when it did not fit.
bool put(char* buf, std::size_t cap, std::size_t& pos, const char* s, std::size_t len) noexcept {
    if (pos + len + 1 > cap) { return false; }
    std::memcpy(buf + pos, s, len);
    pos += len;
    return true;
}

std::size_t putDecimal(char* out, std::uint64_t v) noexcept {
    char tmp[24];
    std::size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    for (std::size_t i = 0; i < n; ++i) { out[i] = tmp[n - 1 - i]; }
    return n;
}

}  // namespace

const char* eventWord(Event e) noexcept {
    const unsigned i = static_cast<unsigned>(e);
    return i >= 1 && i <= kEventCount ? kEventWords[i] : "";
}

unsigned qualifierCount(Event e) noexcept {
    unsigned n = 0;
    tableOf(e, n);
    return n;
}

const char* qualifierWord(Event e, unsigned q) noexcept {
    unsigned n = 0;
    const char* const* t = tableOf(e, n);
    return (t != nullptr && q >= 1 && q <= n) ? t[q - 1] : nullptr;
}

unsigned qualifierIndex(Event e, std::string_view word) noexcept {
    unsigned n = 0;
    const char* const* t = tableOf(e, n);
    for (unsigned i = 0; t != nullptr && i < n; ++i) {
        if (word == t[i]) { return i + 1; }
    }
    return 0;
}

void noteAt(Event e, unsigned q, std::uint64_t ms) noexcept {
    const unsigned ei = static_cast<unsigned>(e);
    if (ei < 1 || ei > kEventCount) { return; }
    const unsigned n = qualifierCount(e);
    // An event with qualifiers is written only WITH one, and one without only without.
    if (n == 0 ? q != 0 : (q < 1 || q > n)) { return; }
    const std::uint64_t word = (ms << 16) | (static_cast<std::uint64_t>(q) << 8) | ei;
    const std::uint64_t at = g_head.fetch_add(1, std::memory_order_relaxed);
    g_slots[at % kEntries].store(word, std::memory_order_relaxed);
}

void note(Event e, unsigned q) noexcept { noteAt(e, q, steadyMs()); }

void noteSource(bool opened, std::string_view driverWord) noexcept {
    const Event e = opened ? Event::SrcOpen : Event::SrcClose;
    unsigned q = qualifierIndex(e, driverWord);
    if (q == 0) { q = qualifierIndex(e, "other"); }
    note(e, q);
}

void noteMode(int index) noexcept {
    if (index < 0 || index >= static_cast<int>(countOf(kModes))) { return; }
    note(Event::Mode, static_cast<unsigned>(index) + 1);
}

void noteRescanStep(const char* phaseName) noexcept {
    if (phaseName == nullptr) { return; }
    for (unsigned i = 0; i < countOf(kStepNames); ++i) {
        if (std::strcmp(kStepNames[i], phaseName) == 0) {
            note(Event::Rescan, i + 1);
            return;
        }
    }
}

std::size_t format(char* buf, std::size_t cap, std::uint64_t nowMs) noexcept {
    if (buf == nullptr || cap == 0) { return 0; }
    buf[0] = '\0';
    const std::uint64_t head = g_head.load(std::memory_order_acquire);
    const std::uint64_t first = head > kEntries ? head - kEntries : 0;
    std::size_t pos = 0;
    bool any = false;
    for (std::uint64_t i = first; i < head; ++i) {
        const std::uint64_t word = g_slots[i % kEntries].load(std::memory_order_relaxed);
        const unsigned ei = static_cast<unsigned>(word & 0xFFu);
        const unsigned q = static_cast<unsigned>((word >> 8) & 0xFFu);
        const std::uint64_t ms = word >> 16;
        if (word == 0 || ei < 1 || ei > kEventCount) { continue; }
        const Event e = static_cast<Event>(ei);
        const unsigned n = qualifierCount(e);
        if (n == 0 ? q != 0 : (q < 1 || q > n)) { continue; }
        std::uint64_t secs = nowMs > ms ? (nowMs - ms) / 1000u : 0;
        if (secs > kMaxSecs) { secs = kMaxSecs; }
        char entry[8 + kMaxEventChars + 2];
        std::size_t len = putDecimal(entry, secs);
        entry[len++] = ':';
        const char* w = kEventWords[ei];
        const std::size_t wl = std::strlen(w);
        std::memcpy(entry + len, w, wl);
        len += wl;
        if (n != 0) {
            const char* qw = qualifierWord(e, q);
            const std::size_t ql = std::strlen(qw);
            entry[len++] = '.';
            std::memcpy(entry + len, qw, ql);
            len += ql;
        }
        if (any && !put(buf, cap, pos, ";", 1)) { break; }
        if (!put(buf, cap, pos, entry, len)) {
            // The separator just written has nothing after it; take it back.
            if (any && pos > 0) { --pos; }
            break;
        }
        any = true;
    }
    buf[pos] = '\0';
    return pos;
}

std::size_t format(char* buf, std::size_t cap) noexcept { return format(buf, cap, steadyMs()); }

std::string formatNow() {
    char buf[kMaxFormatted];
    const std::size_t n = format(buf, sizeof(buf), steadyMs());
    return std::string(buf, n);
}

bool validEntry(std::string_view entry) noexcept {
    // <secs>:
    std::size_t i = 0;
    std::uint64_t secs = 0;
    while (i < entry.size() && entry[i] >= '0' && entry[i] <= '9') {
        secs = secs * 10 + static_cast<std::uint64_t>(entry[i] - '0');
        if (i >= 6) { return false; }   // more than six digits
        ++i;
    }
    if (i == 0 || i >= entry.size() || entry[i] != ':') { return false; }
    if (secs > kMaxSecs) { return false; }
    const std::string_view rest = entry.substr(i + 1);
    const std::size_t dot = rest.find('.');
    const std::string_view ew = rest.substr(0, dot);
    for (unsigned ei = 1; ei <= kEventCount; ++ei) {
        if (ew != kEventWords[ei]) { continue; }
        const Event e = static_cast<Event>(ei);
        if (qualifierCount(e) == 0) { return dot == std::string_view::npos; }
        return dot != std::string_view::npos && qualifierIndex(e, rest.substr(dot + 1)) != 0;
    }
    return false;
}

std::string sanitise(std::string_view text) {
    std::vector<std::string_view> kept;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(';', start);
        if (end == std::string_view::npos) { end = text.size(); }
        const std::string_view piece = text.substr(start, end - start);
        if (validEntry(piece)) { kept.push_back(piece); }
        start = end + 1;
    }
    const std::size_t from = kept.size() > kEntries ? kept.size() - kEntries : 0;
    std::string out;
    for (std::size_t i = from; i < kept.size(); ++i) {
        if (!out.empty()) { out += ';'; }
        out.append(kept[i]);
    }
    return out;
}

void resetForTest() noexcept {
    for (auto& s : g_slots) { s.store(0, std::memory_order_relaxed); }
    g_head.store(0, std::memory_order_relaxed);
}

}  // namespace cascade::core::trail
