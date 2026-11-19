// radio_message.cpp - see radio_message.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_message.hpp"

#include <algorithm>
#include <mutex>

#include "core/analyser_project.hpp"

namespace cascade::core {

std::string radioMessageHash(const std::string& salt, const std::string& radioKey) {
    const std::string joined = salt + radioKey;
    return analyser::sha256Hex(joined.data(), joined.size());
}

bool radioMessageIsFor(const RadioMessage& m, const std::string& radioKey) {
    if (m.salt.empty() || m.hash.size() != 64 || radioKey.empty()) { return false; }
    return radioMessageHash(m.salt, radioKey) == m.hash;
}

namespace {

// The next code point of `s` at `i`, advancing `i`; false (and one byte skipped) for an invalid or truncated sequence, an overlong form, a surrogate or a
// value past U+10FFFF.
bool nextCodePoint(const std::string& s, std::size_t& i, std::uint32_t& cp) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    std::uint32_t v = 0;
    std::uint32_t minimum = 0;
    if (c < 0x80) { cp = c; ++i; return true; }
    if ((c & 0xE0u) == 0xC0u) { len = 2; v = c & 0x1Fu; minimum = 0x80; }
    else if ((c & 0xF0u) == 0xE0u) { len = 3; v = c & 0x0Fu; minimum = 0x800; }
    else if ((c & 0xF8u) == 0xF0u) { len = 4; v = c & 0x07u; minimum = 0x10000; }
    else { ++i; return false; }
    if (i + len > s.size()) { ++i; return false; }
    for (std::size_t k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0u) != 0x80u) { ++i; return false; }
        v = (v << 6) | (cc & 0x3Fu);
    }
    if (v < minimum || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) { ++i; return false; }
    i += len;
    cp = v;
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) { out.push_back(static_cast<char>(cp)); }
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Code points a plain-text message does not carry: C0 controls (but the new line, which is handled before this), DEL, the C1 controls, the zero-width
// and direction marks, the line and paragraph separators, the direction overrides and isolates, the byte-order mark, the specials block.
bool droppedCodePoint(std::uint32_t cp) {
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) || (cp >= 0x200B && cp <= 0x200F) || cp == 0x2028 || cp == 0x2029 ||
           (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || (cp >= 0xFFF0 && cp <= 0xFFFF);
}

}  // namespace

std::string sanitiseRadioMessageText(const std::string& raw, std::size_t maxChars) {
    std::string out;
    std::size_t chars = 0;
    int newlines = 0;
    for (std::size_t i = 0; i < raw.size();) {
        std::uint32_t cp = 0;
        if (!nextCodePoint(raw, i, cp)) { continue; }
        if (cp == '\r') { continue; }
        if (cp == '\n') {
            if (out.empty() || newlines >= 2) { continue; }   // no leading blank lines; a run of blank lines is one
            ++newlines;
        } else if (cp == '\t') {
            cp = ' ';
            newlines = 0;
        } else if (droppedCodePoint(cp)) {
            continue;
        } else {
            newlines = 0;
        }
        if (chars >= maxChars) { break; }
        appendUtf8(out, cp);
        ++chars;
    }
    // trim the ends (spaces and new lines): the clamp may have stopped on one
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) { out.pop_back(); }
    std::size_t lead = 0;
    while (lead < out.size() && (out[lead] == ' ' || out[lead] == '\n')) { ++lead; }
    return out.substr(lead);
}

namespace radiomessage {
namespace {
std::mutex g_mu;
std::vector<KnownRadio> g_radios;
std::uint64_t g_gen = 0;
constexpr std::size_t kMaxKnown = 64;
}  // namespace

void noteRadio(const std::string& key, const std::string& model, const std::string& serial) {
    if (key.empty() || serial.empty() || serial == "-") { return; }
    std::lock_guard<std::mutex> lk(g_mu);
    for (const KnownRadio& r : g_radios) {
        if (r.key == key) { return; }
    }
    if (g_radios.size() >= kMaxKnown) { return; }
    std::string up = serial;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
    g_radios.push_back(KnownRadio{key, model, up});
    ++g_gen;
}

std::vector<KnownRadio> knownRadios() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_radios;
}

std::uint64_t generation() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_gen;
}

void resetForTest() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_radios.clear();
    g_gen = 0;
}
}  // namespace radiomessage

RadioMessagePopup radioMessagePopupFor(bool updateCheckEnabled, bool packaged, const std::vector<RadioMessage>& messages, const std::vector<KnownRadio>& radios,
                                       const std::vector<std::string>& seen) {
    RadioMessagePopup p;
    if (!updateCheckEnabled || packaged) { return p; }
    // oldest first: the list is newest first, so walk it backwards
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (std::find(seen.begin(), seen.end(), it->id) != seen.end()) { continue; }
        const KnownRadio* hit = nullptr;
        for (const KnownRadio& r : radios) {
            if (radioMessageIsFor(*it, r.key)) { hit = &r; break; }
        }
        if (hit == nullptr) { continue; }
        if (!p.shown) {
            p.shown = true;
            p.id = it->id;
            p.date = it->date;
            p.text = it->text;
            p.model = hit->model;
            p.serialTail = hit->serial.size() > 4 ? hit->serial.substr(hit->serial.size() - 4) : hit->serial;
        } else {
            ++p.more;
        }
    }
    return p;
}

}  // namespace cascade::core
