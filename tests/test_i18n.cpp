// Tests for core/i18n.hpp - the translation engine - and for the catalogues
// in resources/lang that it is fed.
//
// THREE HALVES.
//
//   1. THE ENGINE, against catalogues written in this file: English is a
//      passthrough of the very same pointer; a hit translates and a miss is
//      English; trId keeps every widget's ImGui id identical to the id its
//      English label always had (checked with ImGui's own hash, not by
//      reading the strings); pointers survive a language switch; the
//      auto-language rule; malformed catalogues are refused and unsafe
//      entries dropped.
//
//   2. THE SHIPPED CATALOGUES, every resources/lang/*.json: valid UTF-8, no
//      empty translation, no "##" in one, the SAME printf conversions as the
//      key (a mismatch reads the wrong argument - a crash, not a typo), and
//      COVERAGE against the source: every key src/ asks for is present and no
//      key is present that src/ no longer asks for. With no catalogues yet the
//      loop has nothing to check, and the extractor that drives it is tested
//      on its own so it is known to work before the first catalogue arrives.
//
//   3. THE COMPILED-IN BYTES are those files, byte for byte (the catalogues
//      reach the binary through tools/embed-lang.py, exactly as the fonts do).
//
// The key extraction here and in tools/i18n_keys.py follow ONE set of rules;
// a change to either is a change to both.
//
// LOCATING THE REPOSITORY. ctest passes the source root on the command line
// (tests/CMakeLists.txt); run by hand without one, the test walks up from the
// working directory, the same way test_band_plan does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/utf8_text.hpp"

#include "imgui.h"
#include "imgui_internal.h"  // ImHashStr: the hash ImGui keys every widget on
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

std::string g_root;

std::string findRoot() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    for (int level = 0; !ec && level < 10; ++level) {
        if (fs::is_regular_file(dir / "resources" / "bandplans" / "uk.json", ec) &&
            fs::is_directory(dir / "src", ec)) {
            return dir.string();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

bool readExact(const fs::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// --- UTF-8 -------------------------------------------------------------------

// Strict: no overlong forms, no surrogates, nothing past U+10FFFF. Returns the
// byte offset of the first bad sequence, or npos.
std::size_t firstBadUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        unsigned int cp = 0;
        unsigned int min = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c < 0xE0) {
            len = 2;
            cp = c & 0x1Fu;
            min = 0x80;
        } else if (c >= 0xE0 && c < 0xF0) {
            len = 3;
            cp = c & 0x0Fu;
            min = 0x800;
        } else if (c >= 0xF0 && c < 0xF5) {
            len = 4;
            cp = c & 0x07u;
            min = 0x10000;
        } else {
            return i;
        }
        if (i + len > s.size()) { return i; }
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0u) != 0x80u) { return i; }
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { return i; }
        i += len;
    }
    return std::string::npos;
}

void appendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
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

// --- key extraction (the same rules as tools/i18n_keys.py) -------------------
//
// A small C++ lexer: comments are skipped, string and character literals are
// read as literals (so "tr(" inside a string is not a call and a quote inside
// '"' does not open one), and numbers are read whole so a digit separator
// (1'000) is not taken for a character literal. A KEY is a call of tr, trId
// or FOX_TR_NOOP whose whole argument is one string literal or several
// adjacent ones, joined as the compiler joins them. For trId and FOX_TR_NOOP
// the key is the text before "##" (the part a widget shows); an id-only
// literal is no key. A call whose argument is anything else - a variable, a
// table entry - is not a key here: its literal is marked FOX_TR_NOOP where it
// is defined.

struct Token {
    enum Kind { Ident, String, Punct } kind;
    std::string text;
    std::size_t pos = 0;  // byte offset of the token's first character
};

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

int hexVal(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

// Reads an ordinary literal body from src[i] (just past the opening quote) to
// the closing `quote`, decoding escapes. Leaves i just past the closing quote.
std::string readQuoted(const std::string& src, std::size_t& i, char quote) {
    std::string out;
    while (i < src.size() && src[i] != quote) {
        char c = src[i++];
        if (c == '\n') { break; }  // unterminated: stop at the line end
        if (c != '\\' || i >= src.size()) {
            out.push_back(c);
            continue;
        }
        c = src[i++];
        switch (c) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '\\': out.push_back('\\'); break;
            case '\'': out.push_back('\''); break;
            case '"': out.push_back('"'); break;
            case '?': out.push_back('?'); break;
            case '\n': break;  // a line splice
            case '\r':
                if (i < src.size() && src[i] == '\n') { ++i; }
                break;
            case 'x': {
                unsigned long v = 0;
                while (i < src.size() && hexVal(src[i]) >= 0) { v = v * 16 + hexVal(src[i++]); }
                out.push_back(static_cast<char>(v & 0xFF));
                break;
            }
            case 'u':
            case 'U': {
                const int n = (c == 'u') ? 4 : 8;
                unsigned long v = 0;
                for (int k = 0; k < n && i < src.size() && hexVal(src[i]) >= 0; ++k) {
                    v = v * 16 + hexVal(src[i++]);
                }
                appendUtf8(out, v);
                break;
            }
            default:
                if (c >= '0' && c <= '7') {
                    unsigned v = static_cast<unsigned>(c - '0');
                    for (int k = 0; k < 2 && i < src.size() && src[i] >= '0' && src[i] <= '7'; ++k) {
                        v = v * 8 + static_cast<unsigned>(src[i++] - '0');
                    }
                    out.push_back(static_cast<char>(v & 0xFF));
                } else {
                    out.push_back(c);
                }
        }
    }
    if (i < src.size() && src[i] == quote) { ++i; }
    return out;
}

std::vector<Token> lex(const std::string& src) {
    std::vector<Token> toks;
    std::size_t i = 0;
    const std::size_t n = src.size();
    while (i < n) {
        const char c = src[i];
        const std::size_t tokStart = i;
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') {
                // a backslash at the end of a // comment continues it
                if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') { ++i; }
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            const std::size_t end = src.find("*/", i + 2);
            i = (end == std::string::npos) ? n : end + 2;
            continue;
        }
        if (c == '"') {
            ++i;
            toks.push_back({Token::String, readQuoted(src, i, '"'), tokStart});
            continue;
        }
        if (c == '\'') {
            ++i;
            (void)readQuoted(src, i, '\'');
            toks.push_back({Token::Punct, "'", tokStart});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])) != 0)) {
            // A pp-number: digits, letters, '.', digit separators, and a sign
            // straight after an exponent letter.
            ++i;
            while (i < n) {
                const char d = src[i];
                if ((d == '+' || d == '-') &&
                    (src[i - 1] == 'e' || src[i - 1] == 'E' || src[i - 1] == 'p' ||
                     src[i - 1] == 'P')) {
                    ++i;
                } else if (isIdentChar(d) || d == '.' ||
                           (d == '\'' && i + 1 < n && isIdentChar(src[i + 1]))) {
                    ++i;
                } else {
                    break;
                }
            }
            continue;
        }
        if (isIdentStart(c)) {
            const std::size_t start = i;
            while (i < n && isIdentChar(src[i])) { ++i; }
            const std::string id = src.substr(start, i - start);
            if (i < n && src[i] == '"') {
                const bool raw = !id.empty() && id.back() == 'R' &&
                                 (id == "R" || id == "LR" || id == "uR" || id == "UR" || id == "u8R");
                const bool prefix = id == "L" || id == "u" || id == "U" || id == "u8";
                if (raw) {
                    // R"delim( ... )delim"
                    const std::size_t open = src.find('(', i + 1);
                    if (open == std::string::npos) { break; }
                    const std::string delim = src.substr(i + 1, open - i - 1);
                    const std::string close = ")" + delim + "\"";
                    const std::size_t end = src.find(close, open + 1);
                    const std::size_t stop = end == std::string::npos ? n : end;
                    toks.push_back({Token::String, src.substr(open + 1, stop - open - 1), tokStart});
                    i = end == std::string::npos ? n : end + close.size();
                    continue;
                }
                if (prefix) {
                    ++i;
                    toks.push_back({Token::String, readQuoted(src, i, '"'), tokStart});
                    continue;
                }
            }
            toks.push_back({Token::Ident, id, tokStart});
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }
        toks.push_back({Token::Punct, std::string(1, c), tokStart});
        ++i;
    }
    return toks;
}

// Keys used by one source text, in order of appearance.
std::vector<std::string> extractKeys(const std::string& src) {
    std::vector<std::string> keys;
    const std::vector<Token> t = lex(src);
    for (std::size_t k = 0; k < t.size(); ++k) {
        if (t[k].kind != Token::Ident) { continue; }
        const std::string& fn = t[k].text;
        const bool isTr = fn == "tr";
        const bool splitsId = fn == "trId" || fn == "FOX_TR_NOOP";
        if (!isTr && !splitsId) { continue; }
        if (k + 1 >= t.size() || t[k + 1].kind != Token::Punct || t[k + 1].text != "(") { continue; }
        std::size_t j = k + 2;
        std::string joined;
        bool any = false;
        while (j < t.size() && t[j].kind == Token::String) {
            joined += t[j].text;
            any = true;
            ++j;
        }
        if (!any || j >= t.size() || t[j].kind != Token::Punct || t[j].text != ")") { continue; }
        if (splitsId) {
            const std::size_t h = joined.find("##");
            if (h != std::string::npos) { joined.resize(h); }
        }
        if (!joined.empty()) { keys.push_back(joined); }
    }
    return keys;
}

std::set<std::string> sourceKeys(const fs::path& src) {
    std::set<std::string> keys;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) { continue; }
        const std::string ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        std::string text;
        if (!readExact(it->path(), text)) { continue; }
        for (std::string& k : extractKeys(text)) { keys.insert(std::move(k)); }
    }
    return keys;
}

// --- half 1: the engine ------------------------------------------------------

const char* kPtBr = R"j({"code":"pt-BR","name":"Português (Brasil)",
  "englishName":"Portuguese (Brazil)","machine":true,
  "strings":{"Band plan":"Plano de banda","Save":"Salvar","Close":"Fechar",
             "%s (%d bands)":"%s (%d faixas)","Language & country":"Idioma e país"}})j";

const char* kDe = R"j({"code":"de","name":"Deutsch","englishName":"German","machine":false,
  "strings":{"Band plan":"Bandplan","Save":"Speichern"}})j";

void testEnglishPassthrough() {
    std::printf("  English: tr and trId hand back the very same pointer; null in, null out\n");
    static const char kLabel[] = "Band plan";
    static const char kId[] = "Save##row3";
    CHECK(cascade::i18n::current() == "en");
    CHECK(tr(kLabel) == kLabel);
    CHECK(trId(kLabel) == kLabel);
    CHECK(trId(kId) == kId);
    CHECK(tr(nullptr) == nullptr);
    CHECK(trId(nullptr) == nullptr);
    CHECK(cascade::i18n::setLanguage("en") == "en");
    CHECK(tr(kLabel) == kLabel);
    CHECK(cascade::i18n::languages().front().code == "en");
}

void testCatalogueLookup() {
    std::printf("  a catalogue from JSON: hits translate, misses are English, formats whole\n");
    std::string err;
    std::vector<std::string> warn;
    CHECK(cascade::i18n::addCatalogue(kPtBr, &err, &warn));
    CHECK(err.empty());
    CHECK(warn.empty());
    CHECK(cascade::i18n::addCatalogue(kDe, &err));
    CHECK(cascade::i18n::setLanguage("pt-BR") == "pt-BR");
    CHECK(cascade::i18n::current() == "pt-BR");
    CHECK(std::strcmp(tr("Band plan"), "Plano de banda") == 0);
    CHECK(std::strcmp(tr("%s (%d bands)"), "%s (%d faixas)") == 0);
    static const char kMiss[] = "Nothing translates this";
    CHECK(tr(kMiss) == kMiss);  // the same pointer, not a copy
    CHECK(tr(nullptr) == nullptr);

    const cascade::i18n::Language* l = cascade::i18n::findLanguage("pt-BR");
    CHECK(l != nullptr);
    if (l != nullptr) {
        CHECK(l->machine);
        CHECK(l->translated == 5);
        CHECK(l->name == "Português (Brasil)");
    }
    // English first, then by English name: German before Portuguese.
    const auto& langs = cascade::i18n::languages();
    std::vector<std::string> codes;
    for (const auto& x : langs) { codes.push_back(x.code); }
    const auto de = std::find(codes.begin(), codes.end(), "de");
    const auto pt = std::find(codes.begin(), codes.end(), "pt-BR");
    CHECK(!codes.empty() && codes.front() == "en");
    CHECK(de != codes.end() && pt != codes.end() && de < pt);
}

// The property trId exists for, asserted with ImGui's own hash: whatever the
// language, the widget's id is the id its English label has.
bool sameImGuiId(const char* a, const char* b) { return ImHashStr(a, 0, 0) == ImHashStr(b, 0, 0); }

void testTrIdForms() {
    std::printf("  trId: plain, ##suffix, ###id and id-only labels, and the id never moves\n");
    CHECK(cascade::i18n::setLanguage("pt-BR") == "pt-BR");
    const char* plain = trId("Save");
    CHECK(std::strcmp(plain, "Salvar###Save") == 0);
    CHECK(sameImGuiId(plain, "Save"));

    const char* suffixed = trId("Save##row3");
    CHECK(std::strcmp(suffixed, "Salvar###Save##row3") == 0);
    CHECK(sameImGuiId(suffixed, "Save##row3"));

    const char* withId = trId("Language & country###language");
    CHECK(std::strcmp(withId, "Idioma e país###language") == 0);
    CHECK(sameImGuiId(withId, "Language & country###language"));

    static const char kIdOnly[] = "##freq";
    CHECK(trId(kIdOnly) == kIdOnly);
    static const char kUntranslated[] = "Remove##plugin7";
    CHECK(trId(kUntranslated) == kUntranslated);
    static const char kUntranslatedPlain[] = "Not in any catalogue";
    CHECK(trId(kUntranslatedPlain) == kUntranslatedPlain);

    // Cached: the second call is the same storage, not a new string.
    CHECK(trId("Save") == plain);
    CHECK(trId("Save##row3") == suffixed);
}

void testPointersSurviveSwitching() {
    std::printf("  pointers from tr/trId stay valid and unchanged across language switches\n");
    CHECK(cascade::i18n::setLanguage("pt-BR") == "pt-BR");
    const char* p = tr("Band plan");
    const char* id = trId("Save");
    CHECK(cascade::i18n::setLanguage("de") == "de");
    CHECK(std::strcmp(tr("Band plan"), "Bandplan") == 0);
    CHECK(std::strcmp(trId("Save"), "Speichern###Save") == 0);
    CHECK(std::strcmp(p, "Plano de banda") == 0);  // still readable, still itself
    CHECK(std::strcmp(id, "Salvar###Save") == 0);
    CHECK(cascade::i18n::setLanguage("en") == "en");
    CHECK(std::strcmp(p, "Plano de banda") == 0);
    CHECK(cascade::i18n::setLanguage("pt-BR") == "pt-BR");
    CHECK(tr("Band plan") == p);  // back again: the very same pointer
    CHECK(trId("Save") == id);

    // A REPLACED catalogue is kept alive too: re-adding pt-BR moves lookups to
    // the new one while everything handed out from the old one stays readable.
    std::string replaced = kPtBr;
    const std::size_t at = replaced.find("Plano de banda");
    replaced.replace(at, std::strlen("Plano de banda"), "Plano de bandas");
    std::string err;
    CHECK(cascade::i18n::addCatalogue(replaced, &err));
    CHECK(cascade::i18n::current() == "pt-BR");  // the active one was swapped in place
    CHECK(std::strcmp(tr("Band plan"), "Plano de bandas") == 0);
    CHECK(std::strcmp(p, "Plano de banda") == 0);
    CHECK(cascade::i18n::addCatalogue(kPtBr, &err));  // restore for later tests
}

void testResolve() {
    using cascade::i18n::resolveFor;
    std::printf("  resolveFor: exact tag, primary subtag, unknowns to English, any case\n");
    CHECK(resolveFor("auto", "pt-BR") == "pt-BR");
    // pt-PT has its own catalogue since the 27-language release, so it is an
    // exact match now; the primary-subtag rule is exercised by de-AT below.
    CHECK(resolveFor("auto", "pt-PT") == "pt-PT");
    CHECK(resolveFor("auto", "PT-br") == "pt-BR");
    CHECK(resolveFor("AUTO", "de-AT") == "de");
    CHECK(resolveFor("", "de-CH") == "de");
    CHECK(resolveFor("auto", "xx-YY") == "en");
    CHECK(resolveFor("auto", "") == "en");
    CHECK(resolveFor("auto", "en-GB") == "en");
    CHECK(resolveFor("de", "pt-BR") == "de");  // an explicit choice ignores the OS
    CHECK(resolveFor("DE", "") == "de");
    CHECK(resolveFor("pt-br", "") == "pt-BR");  // returned in its canonical spelling
    CHECK(resolveFor("klingon", "pt-BR") == "en");
    CHECK(resolveFor("pt", "") == "en");  // explicit codes match exactly, not by prefix
    CHECK(resolveFor("en", "pt-BR") == "en");

    // Two catalogues of one language: the bare language wins a regional
    // desktop that matches neither exactly.
    std::string err;
    const char* esMx =
        R"j({"code":"es-MX","name":"Español (México)","englishName":"Spanish (Mexico)","strings":{}})j";
    const char* es = R"j({"code":"es","name":"Español","englishName":"Spanish","strings":{}})j";
    CHECK(cascade::i18n::addCatalogue(esMx, &err));
    CHECK(cascade::i18n::addCatalogue(es, &err));
    CHECK(resolveFor("auto", "es-AR") == "es");
    CHECK(resolveFor("auto", "es-MX") == "es-MX");
    CHECK(cascade::i18n::matchCatalogue("pt-PT") == "pt-PT");
    CHECK(cascade::i18n::matchCatalogue("de-AT") == "de");  // primary subtag
    CHECK(cascade::i18n::matchCatalogue("en-US").empty());
    CHECK(cascade::i18n::matchCatalogue("").empty());
}

void testMalformed() {
    std::printf("  malformed catalogues are refused with a reason; unsafe entries dropped\n");
    const std::size_t before = cascade::i18n::languages().size();
    const char* bad[] = {
        "",
        "{",
        "[]",
        R"j({"code":"fr","name":"Français","englishName":"French"})j",  // no strings
        R"j({"name":"Français","englishName":"French","strings":{}})j",  // no code
        R"j({"code":"en","name":"English","englishName":"English","strings":{}})j",
        R"j({"code":"f","name":"x","englishName":"x","strings":{}})j",
        R"j({"code":"fr","name":"x","englishName":"x","strings":{"a":7}})j",
        R"j({"code":"fr","name":"x","englishName":"x","machine":"yes","strings":{}})j",
        R"j({"code":"fr","name":"","englishName":"x","strings":{}})j",
        "{\"code\":\"fr\",\"name\":\"\xC3\x28\",\"englishName\":\"x\",\"strings\":{}}",
    };
    for (const char* b : bad) {
        std::string err;
        const bool ok = cascade::i18n::addCatalogue(b, &err);
        CHECK(!ok);
        CHECK(!err.empty());
        if (ok) { std::printf("      accepted: %s\n", b); }
    }
    CHECK(cascade::i18n::languages().size() == before);

    std::string err;
    std::vector<std::string> warn;
    const char* frText = R"j({"code":"fr","name":"Français","englishName":"French","strings":{
            "%d bands":"%s bandes","Save":"","Close":"Fermer##x","OK key":"Bien"}})j";
    CHECK(cascade::i18n::addCatalogue(frText, &err, &warn));
    CHECK(warn.size() == 3);
    CHECK(cascade::i18n::setLanguage("fr") == "fr");
    static const char kBands[] = "%d bands";
    CHECK(tr(kBands) == kBands);  // the mismatched entry was left out: English
    CHECK(std::strcmp(tr("OK key"), "Bien") == 0);
    const cascade::i18n::Language* fr = cascade::i18n::findLanguage("fr");
    CHECK(fr != nullptr && fr->translated == 1);
    CHECK(cascade::i18n::setLanguage("en") == "en");
}

// FOXSDR_LANG_FILE's path: a catalogue a translator saved to disk. What
// matters is that a good file is added and selectable by its code, that a
// file with a byte-order mark is still good, and that every failure says so
// on the diagnostic stream with the file's name - silence would leave the
// translator looking at English and wondering what they did wrong.
void testCatalogueFile() {
    std::printf("  addCatalogueFile: loads a file, skips a BOM, names every problem\n");
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
                         ("foxsdr_test_i18n_file_" + std::to_string(std::rand()));
    fs::create_directories(dir, ec);
    CHECK(!ec);
    const fs::path diagPath = dir / "diag.txt";
    auto write = [&](const char* name, const std::string& body) {
        std::ofstream f(dir / name, std::ios::binary);
        f << body;
        return (dir / name).string();
    };
    // Runs one load and returns (code, what it wrote to the diagnostic stream).
    auto load = [&](const std::string& path) {
        std::FILE* diag = std::fopen(diagPath.string().c_str(), "wb");
        CHECK(diag != nullptr);
        const std::string code = cascade::i18n::addCatalogueFile(path, diag);
        if (diag != nullptr) { std::fclose(diag); }
        std::string said;
        readExact(diagPath, said);
        return std::make_pair(code, said);
    };

    const std::string good = write("xx.json", R"j({"code":"qps-ploc","name":"Pseudo",
      "englishName":"Pseudo","strings":{"Band plan":"[Bánd płán ~~~]","Save":"",
      "%d bands":"%s bands"}})j");
    const auto [code, said] = load(good);
    CHECK(code == "qps-ploc");
    // The two entries left in English are named, each with the file.
    CHECK(said.find("\"Save\" has no translation") != std::string::npos);
    CHECK(said.find("\"%d bands\"") != std::string::npos);
    CHECK(said.find(good) != std::string::npos);
    CHECK(cascade::i18n::findLanguage("qps-ploc") != nullptr);
    CHECK(cascade::i18n::setLanguage("qps-ploc") == "qps-ploc");
    CHECK(std::strcmp(tr("Band plan"), "[Bánd płán ~~~]") == 0);
    CHECK(cascade::i18n::setLanguage("en") == "en");

    // A byte-order mark in front of an otherwise good document.
    const std::string bom = write("bom.json", "\xEF\xBB\xBF" R"j({"code":"qps-plocm",
      "name":"Pseudo M","englishName":"Pseudo M","strings":{"Band plan":"[B]"}})j");
    const auto [bomCode, bomSaid] = load(bom);
    CHECK(bomCode == "qps-plocm");
    CHECK(bomSaid.empty());

    // Failures: a file that is not there, and one that is not a catalogue.
    const auto [noCode, noSaid] = load((dir / "absent.json").string());
    CHECK(noCode.empty());
    CHECK(noSaid.find("absent.json") != std::string::npos);
    const auto [badCode, badSaid] = load(write("bad.json", "{\"code\":\"qps-x\""));
    CHECK(badCode.empty());
    CHECK(badSaid.find("not valid JSON") != std::string::npos);
    CHECK(cascade::i18n::findLanguage("qps-x") == nullptr);

    fs::remove_all(dir, ec);
}

void testFormatSpecs() {
    using cascade::i18n::formatSpecs;
    using V = std::vector<std::string>;
    std::printf("  formatSpecs parses flags, width, precision, length and %%%%\n");
    CHECK(formatSpecs("%s (%d bands)") == V({"%s", "%d"}));
    CHECK(formatSpecs("100%% sure") == V());
    CHECK(formatSpecs("%-5.2f|%zu|%lld|%I64d|%*d|%.*s|%1$s|%#x|%+.3e|%hhu|%%|%c") ==
          V({"%-5.2f", "%zu", "%lld", "%I64d", "%*d", "%.*s", "%1$s", "%#x", "%+.3e", "%hhu",
             "%c"}));
    CHECK(formatSpecs("ends with %") == V({"%?"}));
    CHECK(formatSpecs("%q") == V({"%?q"}));
    CHECK(formatSpecs("%5.1f MHz") != formatSpecs("%.1f MHz"));
    CHECK(formatSpecs(nullptr).empty());
}

void testLocale() {
    using cascade::i18n::localeFromPosix;
    std::printf("  POSIX locale values normalise to language tags\n");
    CHECK(localeFromPosix("pt_BR.UTF-8") == "pt-BR");
    CHECK(localeFromPosix("de_DE@euro") == "de-DE");
    CHECK(localeFromPosix("fr") == "fr");
    CHECK(localeFromPosix("C").empty());
    CHECK(localeFromPosix("C.UTF-8").empty());
    CHECK(localeFromPosix("POSIX").empty());
    CHECK(localeFromPosix("").empty());

    // Whatever the machine says, it comes back as a tag or not at all.
    const std::string sys = cascade::i18n::systemLocale();
    std::printf("      this machine's UI language: \"%s\"\n", sys.c_str());
    CHECK(sys.find('_') == std::string::npos);
    CHECK(sys.find('.') == std::string::npos);
#ifndef _WIN32
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    setenv("LANG", "pl_PL.UTF-8", 1);
    CHECK(cascade::i18n::systemLocale() == "pl-PL");
    setenv("LC_MESSAGES", "C", 1);  // set, and "C": no language
    CHECK(cascade::i18n::systemLocale().empty());
    setenv("LC_ALL", "it_IT", 1);
    CHECK(cascade::i18n::systemLocale() == "it-IT");
#endif
}

// --- the extractor itself, before any catalogue depends on it ----------------

// One extraction case: `src` must yield exactly `want`. A helper rather than
// CHECK(extractKeys(R"(...)") == ...) because MSVC cannot stringize a raw
// literal inside a macro argument, and a failure should show what came back.
void expectKeys(const std::string& src, const std::vector<std::string>& want) {
    const std::vector<std::string> got = extractKeys(src);
    if (got != want) {
        std::printf("      extractor on [%s] gave %zu keys:", src.c_str(), got.size());
        for (const std::string& g : got) { std::printf(" [%s]", g.c_str()); }
        std::printf("\n");
    }
    CHECK(got == want);
}

void testExtractor() {
    using V = std::vector<std::string>;
    std::printf("  the key extractor: literals, escapes, joins, comments, ids\n");
    const char bs = '\\';  // built from a char so no tool or escape eats it
    expectKeys(R"x(ImGui::TextUnformatted(tr("Band plan"));)x", V({"Band plan"}));

    // Every escape form the compiler decodes, decoded the same way.
    std::string esc = "tr(\"a";
    esc += bs; esc += "tb";       // \t
    esc += bs; esc += "\"c";      // \"
    esc += bs; esc += bs; esc += "d";  // \\ .
    esc += bs; esc += "x41";      // \x41 = A
    esc += bs; esc += "101";      // \101 = A
    esc += bs; esc += "u00e9";    // é = e-acute, as UTF-8
    esc += "\")";
    expectKeys(esc, V({std::string("a\tb\"c") + bs + "dAA\xC3\xA9"}));

    expectKeys("tr(\"one \"\n   \"two \"  \"three\")", V({"one two three"}));
    expectKeys("// tr(\"in a comment\")\n  /* tr(\"in a block\") */ tr(\"real\")", V({"real"}));
    expectKeys(R"x(trId("Save##row"); trId("##idonly"); trId("A###b");)x", V({"Save", "A"}));
    expectKeys(R"x(FOX_TR_NOOP("Brazil"), FOX_TR_NOOP("Row##x"))x", V({"Brazil", "Row"}));
    expectKeys(R"x(str("no"); attr("no"); tr(kTable[i]); tr("a" + x);)x", V());
    std::string inString = "const char* s = \"tr(";
    inString += bs; inString += "\"not a call";
    inString += bs; inString += "\")\"; tr ( \"spaced\" )";
    expectKeys(inString, V({"spaced"}));
    expectKeys(R"x(char q = '"'; int n = 1'000'000; tr("after"))x", V({"after"}));
    std::string raw = "tr(R\"y(raw ";
    raw += bs; raw += "n \"quoted\")y\")";
    expectKeys(raw, V({std::string("raw ") + bs + "n \"quoted\""}));
    expectKeys("tr(\"%s (%d bands)\")", V({"%s (%d bands)"}));
    expectKeys("i18n::tr(\"qualified\")", V({"qualified"}));
    expectKeys("#define FOX_TR_NOOP(s) s", V());
    expectKeys("tr(\"crlf\")\r\n// tr(\"no\")\r\ntr(\"yes\")", V({"crlf", "yes"}));
}

// --- no translated sentence is formatted into a fixed buffer -----------------
//
// A sentence sized for English is cut in Russian: Cyrillic, Greek and
// Vietnamese take two bytes a letter and Chinese three, so "RUNNING" in a
// 16-byte chip or an aircraft-timeout sentence in 320 bytes lost its end on
// exactly the screens the translation was made for. The engine's answer is
// the std::string form of formatUtf8 (core/utf8_text.hpp), which is sized by
// the text. This check keeps it that way: an snprintf, std::snprintf,
// vsnprintf or formatUtf8 whose SECOND argument is a sizeof - the fixed-buffer
// shape - must not have a tr()/trId() call among its arguments, and its format
// must be a string literal (or a choice between literals), because a format
// held in a variable is how a translated one arrives unseen. A buffer sized
// by the caller instead - snprintf(out, n, ...) into a parameter, formatUtf8
// into a vector's .size() - must not take a tr() either. The few places that
// are genuinely fine are named below, each with its reason.
//
// What it cannot see: a translated string already sitting in a variable and
// copied in through "%s". Those were converted by hand; this catches the
// shapes that can be caught by reading.

struct FixedFormatSite {
    std::size_t line;
    std::string function;  // innermost enclosing named function, "" at file scope
};

bool isKeyword(const std::string& s) {
    static const std::set<std::string> kw = {"if",       "for",      "while",   "switch",
                                             "catch",    "return",   "sizeof",  "decltype",
                                             "noexcept", "alignof",  "alignas", "static_assert"};
    return kw.count(s) != 0;
}

// For the '{' at t[k]: the name of the function whose body it opens, or ""
// when it opens anything else (a namespace, a class, an initialiser, a
// lambda, an if). Reads back to the ')' of a parameter list and takes the
// identifier in front of its '('.
std::string functionOpenedBy(const std::vector<Token>& t, std::size_t k) {
    std::size_t j = k;
    for (int steps = 0; j > 0 && steps < 16; ++steps) {
        --j;
        const Token& x = t[j];
        if (x.kind == Token::Punct && x.text == ")") { break; }
        if (x.kind == Token::Punct && (x.text == ";" || x.text == "{" || x.text == "}" ||
                                       x.text == "=" || x.text == ",")) {
            return {};
        }
        if (j == 0) { return {}; }
    }
    if (t[j].kind != Token::Punct || t[j].text != ")") { return {}; }
    int depth = 0;
    for (;;) {
        if (t[j].kind == Token::Punct && t[j].text == ")") { ++depth; }
        if (t[j].kind == Token::Punct && t[j].text == "(") {
            if (--depth == 0) { break; }
        }
        if (j == 0) { return {}; }
        --j;
    }
    if (j == 0 || t[j - 1].kind != Token::Ident || isKeyword(t[j - 1].text)) { return {}; }
    return t[j - 1].text;
}

// Every fixed-buffer format call in one source text that could carry a
// translated sentence.
std::vector<FixedFormatSite> fixedFormatSites(const std::string& src) {
    std::vector<FixedFormatSite> out;
    const std::vector<Token> t = lex(src);
    std::vector<std::string> scope;  // one entry per open brace: its function
    auto isP = [&](std::size_t k, const char* p) {
        return k < t.size() && t[k].kind == Token::Punct && t[k].text == p;
    };
    for (std::size_t k = 0; k < t.size(); ++k) {
        if (isP(k, "{")) {
            std::string fn = functionOpenedBy(t, k);
            if (fn.empty() && !scope.empty()) { fn = scope.back(); }
            scope.push_back(fn);
            continue;
        }
        if (isP(k, "}")) {
            if (!scope.empty()) { scope.pop_back(); }
            continue;
        }
        if (t[k].kind != Token::Ident) { continue; }
        const std::string& fn = t[k].text;
        if (fn != "snprintf" && fn != "vsnprintf" && fn != "formatUtf8") { continue; }
        if (!isP(k + 1, "(")) { continue; }
        // The arguments, split at top-level commas.
        std::vector<std::vector<std::size_t>> args(1);
        int depth = 0;
        std::size_t j = k + 2;
        for (; j < t.size(); ++j) {
            const Token& x = t[j];
            if (x.kind == Token::Punct && (x.text == "(" || x.text == "[" || x.text == "{")) {
                ++depth;
            } else if (x.kind == Token::Punct && (x.text == ")" || x.text == "]" || x.text == "}")) {
                if (depth == 0) { break; }
                --depth;
            } else if (depth == 0 && x.kind == Token::Punct && x.text == ",") {
                args.emplace_back();
                continue;
            }
            args.back().push_back(j);
        }
        if (args.size() < 3) { continue; }
        // SIZED: the size is a sizeof - a buffer whose length was fixed when
        // the English was written. BOUNDED: any other caller-sized buffer - an
        // snprintf into a (char* out, size_t n) parameter, or a formatUtf8 into
        // a vector's .size(). A bounded buffer may legitimately take a
        // variable format (the helpers that measure the text first and size
        // the vector from it), so only a tr() call in it is flagged.
        bool sized = false;
        bool bounded = fn != "formatUtf8";
        for (std::size_t a : args[1]) {
            sized = sized || (t[a].kind == Token::Ident && t[a].text == "sizeof");
            bounded = bounded || (t[a].kind == Token::Ident && t[a].text == "size" && isP(a + 1, "("));
        }
        if (!sized && !bounded) { continue; }
        bool translated = false;
        for (std::size_t n = 2; n < args.size(); ++n) {
            for (std::size_t a : args[n]) {
                if (t[a].kind == Token::Ident && (t[a].text == "tr" || t[a].text == "trId") &&
                    isP(a + 1, "(")) {
                    translated = true;
                }
            }
        }
        // A literal format: string literals only, or `cond ? "a" : "b"` whose
        // branches are.
        const std::vector<std::size_t>& f = args[2];
        std::size_t from = 0;
        for (std::size_t n = 0; n < f.size(); ++n) {
            if (t[f[n]].kind == Token::Punct && t[f[n]].text == "?") { from = n + 1; }
        }
        bool literal = !f.empty() && from < f.size();
        for (std::size_t n = from; n < f.size(); ++n) {
            const Token& x = t[f[n]];
            const bool colon = from > 0 && x.kind == Token::Punct && x.text == ":";
            literal = literal && (x.kind == Token::String || colon);
        }
        if (translated || (sized && !literal)) {
            const std::size_t line =
                1 + static_cast<std::size_t>(std::count(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(t[k].pos), '\n'));
            out.push_back({line, scope.empty() ? std::string() : scope.back()});
        }
    }
    return out;
}

void testFixedFormatScanner() {
    std::printf("  the fixed-buffer scanner: finds tr()/variable formats into sizeof'd buffers\n");
    auto count = [](const std::string& s) { return fixedFormatSites(s).size(); };
    const auto one = fixedFormatSites("namespace n {\nvoid draw(int x) {\n char b[8];\n"
                                      " formatUtf8(b, sizeof(b), tr(\"x %d\"), x);\n}\n}");
    CHECK(one.size() == 1);
    CHECK(!one.empty() && one[0].line == 4 && one[0].function == "draw");
    CHECK(count("std::snprintf(b, sizeof b, \"%s\", cascade::i18n::tr(\"x\"));") == 1);
    CHECK(count("snprintf(b, sizeof b, fmt, 3);") == 1);
    CHECK(count("snprintf(b, sizeof(b), c ? tr(\"a\") : tr(\"b\"), 3);") == 1);
    CHECK(count("snprintf(b, sizeof b, \"%d\", 3);") == 0);
    CHECK(count("snprintf(b, sizeof b, \"%d\" \" joined\", 3);") == 0);
    CHECK(count("snprintf(b, sizeof(b), u == k ? \"%.0f\" : \"%.0f dB\", v);") == 0);
    CHECK(count("formatUtf8(s, tr(\"x %d\"), 1);") == 0);             // the string form
    // Caller-sized buffers: a tr() is flagged, a measured variable format is not.
    CHECK(count("std::snprintf(out, n, cascade::i18n::tr(\"%zu PORTS\"), c);") == 1);
    CHECK(count("formatUtf8(buf.data(), buf.size(), tr(\"Removed %s\"), f);") == 1);
    CHECK(count("formatUtf8(buf.data(), buf.size(), fmt, f);") == 0);
    CHECK(count("std::snprintf(out, n, \"%.3f\", hz);") == 0);
    CHECK(count("formatUtf8(s, fmt, 1);") == 0);                      // the string form
    CHECK(count("formatUtf8(b, 64, tr(\"x %d\"), 1);") == 0);        // a literal size: not this shape
    CHECK(count("// snprintf(b, sizeof b, fmt, 1);\n/* formatUtf8(b, sizeof b, tr(\"x\")) */") == 0);
    CHECK(count("const char* s = \"snprintf(b, sizeof b, fmt)\";") == 0);
    // A lambda's body belongs to the function around it; an if does not name one.
    const auto lam = fixedFormatSites(
        "std::string outer(int a) {\n auto f = [](const char* fmt) {\n char b[9];\n"
        " if (a) { snprintf(b, sizeof b, fmt, 1); }\n };\n}");
    CHECK(lam.size() == 1 && lam[0].function == "outer");
}

void testNoTranslatedFixedBuffers() {
    // file (relative to the root, forward slashes) + function -> why it is fine
    struct Allowed {
        const char* file;
        const char* function;
        const char* why;
    };
    static const Allowed kAllowed[] = {
        {"src/core/diag_log.cpp", "writef", "the diagnostic log's printf: formats are English log text, never tr()"},
        {"src/core/diag_log.cpp", "diagLogf", "the diagnostic log's printf: formats are English log text, never tr()"},
        {"src/core/diag_log.cpp", "diagWarnf", "the diagnostic log's printf: formats are English log text, never tr()"},
        {"src/core/gps_reader.cpp", "fmt", "English-only status formats, every caller a literal; bounded on purpose"},
        {"src/gui/track_metrics.hpp", "detailPrintf", "English-only detail lines, every caller a literal; bounded on purpose"},
    };
    std::printf("  no tr() text or variable format is printed into a sizeof'd buffer in src/\n");
    const fs::path src = fs::path(g_root) / "src";
    std::set<std::string> allowedUsed;
    std::size_t sites = 0;
    std::size_t files = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) { continue; }
        const std::string ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        std::string text;
        if (!readExact(it->path(), text)) { continue; }
        ++files;
        std::string rel = fs::relative(it->path(), g_root, ec).generic_string();
        for (const FixedFormatSite& s : fixedFormatSites(text)) {
            bool allowed = false;
            for (const Allowed& a : kAllowed) {
                if (rel == a.file && s.function == a.function) {
                    allowed = true;
                    allowedUsed.insert(std::string(a.file) + ":" + a.function);
                }
            }
            if (allowed) { continue; }
            std::printf("      %s:%zu (in %s): a translated or variable format into a fixed buffer - "
                        "use formatUtf8(std::string&, ...)\n",
                        rel.c_str(), s.line, s.function.empty() ? "file scope" : s.function.c_str());
            ++sites;
        }
    }
    std::printf("      %zu files read, %zu offending sites\n", files, sites);
    CHECK(files > 100);  // it read the real tree
    CHECK(sites == 0);
    // An allow-list entry that matches nothing is stale: the place moved or
    // was fixed, and the entry would silently excuse whatever lands there next.
    for (const Allowed& a : kAllowed) {
        const std::string key = std::string(a.file) + ":" + a.function;
        if (allowedUsed.count(key) == 0) { std::printf("      stale allow-list entry: %s\n", key.c_str()); }
        CHECK(allowedUsed.count(key) == 1);
    }
}

// --- half 2: the shipped catalogues -----------------------------------------

std::vector<fs::path> catalogueFiles() {
    std::vector<fs::path> out;
    const fs::path dir = fs::path(g_root) / "resources" / "lang";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) { return out; }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".json") { out.push_back(e.path()); }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    return out;
}

void testShippedCatalogues() {
    const std::vector<fs::path> files = catalogueFiles();
    std::printf("  shipped catalogues: %zu in resources/lang\n", files.size());
    const std::set<std::string> used = sourceKeys(fs::path(g_root) / "src");
    std::printf("      the source asks for %zu keys\n", used.size());
    // The extractor must be reading the real tree, not an empty one: these
    // keys are in src/ today (the country table and the settings section).
    CHECK(used.count("Brazil") == 1);
    CHECK(used.count("Language") == 1);
    CHECK(used.count("Language & country") == 1);
    CHECK(used.count("Automatic (%s)") == 1);

    for (const fs::path& path : files) {
        const std::string file = path.filename().string();
        std::string bytes;
        CHECK(readExact(path, bytes));
        const std::size_t bad = firstBadUtf8(bytes);
        if (bad != std::string::npos) { std::printf("      %s: not UTF-8 at byte %zu\n", file.c_str(), bad); }
        CHECK(bad == std::string::npos);
        const nlohmann::json j = nlohmann::json::parse(bytes, nullptr, false);
        CHECK(!j.is_discarded() && j.is_object());
        if (j.is_discarded() || !j.is_object()) { continue; }
        const std::string code = j.value("code", std::string());
        CHECK(file == code + ".json");
        CHECK(!j.value("name", std::string()).empty());
        CHECK(!j.value("englishName", std::string()).empty());
        const auto strings = j.find("strings");
        CHECK(strings != j.end() && strings->is_object());
        if (strings == j.end() || !strings->is_object()) { continue; }
        std::set<std::string> have;
        for (auto it = strings->begin(); it != strings->end(); ++it) {
            const std::string& key = it.key();
            have.insert(key);
            if (!it.value().is_string()) {
                std::printf("      %s: \"%s\" is not a string\n", file.c_str(), key.c_str());
                CHECK(false);
                continue;
            }
            const std::string value = it.value().get<std::string>();
            if (value.empty()) {
                std::printf("      %s: \"%s\" has an empty translation\n", file.c_str(), key.c_str());
            }
            CHECK(!value.empty());
            if (value.find("##") != std::string::npos) {
                std::printf("      %s: \"%s\" has ## in its translation\n", file.c_str(), key.c_str());
            }
            CHECK(value.find("##") == std::string::npos);
            const auto ks = cascade::i18n::formatSpecs(key.c_str());
            const auto vs = cascade::i18n::formatSpecs(value.c_str());
            if (ks != vs) {
                std::printf("      %s: \"%s\" -> \"%s\": printf conversions differ\n", file.c_str(),
                            key.c_str(), value.c_str());
            }
            CHECK(ks == vs);
        }
        // COVERAGE, both directions, every offender named.
        std::size_t missing = 0;
        for (const std::string& k : used) {
            if (have.count(k) == 0) {
                if (missing < 40) { std::printf("      %s lacks: \"%s\"\n", file.c_str(), k.c_str()); }
                ++missing;
            }
        }
        std::size_t stale = 0;
        for (const std::string& k : have) {
            if (used.count(k) == 0) {
                if (stale < 40) {
                    std::printf("      %s has a key the source no longer uses: \"%s\"\n",
                                file.c_str(), k.c_str());
                }
                ++stale;
            }
        }
        if (missing + stale > 0) {
            std::printf("      %s: %zu missing, %zu stale (py -3.14 tools/i18n_keys.py lists them all)\n",
                        file.c_str(), missing, stale);
        }
        CHECK(missing == 0);
        CHECK(stale == 0);
    }
}

// --- half 3: the compiled-in bytes are the files ------------------------------

void testEmbeddedMatchesFiles() {
    std::printf("  the compiled-in catalogues are resources/lang, byte for byte\n");
    const std::vector<fs::path> files = catalogueFiles();
    const std::vector<cascade::i18n::EmbeddedFile> baked = cascade::i18n::embeddedCatalogueFiles();
    bool same = files.size() == baked.size();
    for (std::size_t i = 0; same && i < files.size(); ++i) {
        std::string disk;
        same = readExact(files[i], disk) && files[i].filename().string() == baked[i].name &&
               disk == std::string(reinterpret_cast<const char*>(baked[i].bytes), baked[i].len);
    }
    if (!same) {
        std::printf("      src/core/lang_assets.hpp is stale (%zu embedded, %zu on disk)\n",
                    baked.size(), files.size());
        std::printf("      re-run from the repository root: py -3.14 tools/embed-lang.py\n");
    }
    CHECK(same);
}

// --- half 4: Chinese and Japanese are not set with spaces between words -------
//
// A catalogue value is often a TEMPLATE, and a translator writing one leaves a
// space either side of "%s" by habit - right when the argument is a device name
// ("%s 已插入"), wrong when it is itself a translated phrase: the census
// sentence "这里没有 %s。" filled with "飞机位置" put a space in the middle of
// a Chinese sentence, "没有 飞机位置" (34-language screenshot review). The
// templates whose FIRST argument is a translated phrase are listed here; in
// them "%s" meets a CJK neighbour with no space between. And no value may put
// a space between two CJK characters at all.
bool cjkChar(unsigned int c) {
    return (c >= 0x3001 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF) ||
           (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xFF01 && c <= 0xFF60);
}

unsigned int cpBeforeByte(const std::string& s, std::size_t i) {
    if (i == 0) { return 0; }
    std::size_t k = i - 1;
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0u) == 0x80u) { --k; }
    return cascade::core::utf8Decode(s, k);
}

unsigned int cpAtByte(const std::string& s, std::size_t i) {
    if (i >= s.size()) { return 0; }
    return cascade::core::utf8Decode(s, i);
}

void testCjkTemplateJoins() {
    std::printf("  zh-CN / zh-TW / ja: no space between CJK words, nor beside a translated %%s\n");
    // The keys whose first %s is filled with a TRANSLATED phrase: the census
    // subject ("aircraft positions", "target positions") and the key-binding
    // clash ("also <action name>").
    const char* const kTranslatedArg[] = {
        "No fitted module publishes tracks of any kind, so there are no %s here. %s",
        "Nothing is publishing %s: \"%s\" is a track source and you stopped it.",
        "Nothing is publishing %s: %d fitted track sources are stopped.",
        "Nothing is publishing %s: \"%s\" is a track source and the host refused it.",
        "Nothing is publishing %s: %d fitted track sources were refused.",
        "Nothing is publishing %s: a fitted track source did not start - the host asked it for one "
        "and was given none. Nothing needs fetching; the module itself failed.",
        "also %s",
    };
    int faults = 0;
    for (const fs::path& path : catalogueFiles()) {
        const std::string code = path.stem().string();
        if (code != "zh-CN" && code != "zh-TW" && code != "ja") { continue; }
        std::string bytes;
        CHECK(readExact(path, bytes));
        const nlohmann::json j = nlohmann::json::parse(bytes, nullptr, false);
        if (j.is_discarded() || !j.contains("strings")) {
            CHECK(false);
            continue;
        }
        const auto& strings = j["strings"];
        for (const char* key : kTranslatedArg) {
            const auto it = strings.find(key);
            CHECK(it != strings.end());
            if (it == strings.end()) { continue; }
            const std::string v = it->get<std::string>();
            const std::size_t at = v.find("%s");
            CHECK(at != std::string::npos);
            if (at == std::string::npos) { continue; }
            const bool before = at >= 1 && v[at - 1] == ' ' && cjkChar(cpBeforeByte(v, at - 1));
            const bool after = at + 3 <= v.size() && v[at + 2] == ' ' && cjkChar(cpAtByte(v, at + 3));
            if (before || after) {
                std::printf("      %s: \"%s\" -> \"%s\": a space beside a translated %%s\n", code.c_str(),
                            key, v.c_str());
                ++faults;
            }
        }
        for (auto it = strings.begin(); it != strings.end(); ++it) {
            if (!it->is_string()) { continue; }
            const std::string v = it->get<std::string>();
            for (std::size_t i = 1; i + 1 < v.size(); ++i) {
                if (v[i] == ' ' && cjkChar(cpBeforeByte(v, i)) && cjkChar(cpAtByte(v, i + 1))) {
                    std::printf("      %s: \"%s\" -> \"%s\": a space between two CJK characters\n",
                                code.c_str(), it.key().c_str(), v.c_str());
                    ++faults;
                    break;
                }
            }
        }
    }
    CHECK(faults == 0);
}

// THE THEMES' WORDS SAY DIFFERENT THINGS IN EVERY LANGUAGE. "Enlarge figures"
// (the counter menu's switch) and "Enlarged figures" (the Display row's size,
// beside "Normal size") sit a menu apart and mean different things - and the
// first Chinese catalogues gave both the same four characters, so the two
// controls read as one (repair round, 2026-09-25). Every key the six themes
// added must translate to a string no other of them uses.
void testThemeWordsDistinct() {
    std::printf("  the themes' own words translate to distinct strings in every catalogue\n");
    const char* const kThemeKeys[] = {
        "Bench Classic XL", "Counter face", "Counter figures", "Daylight Lab",
        "Enlarge every reading", "Enlarge figures", "Enlarged figures", "Field Radio",
        "Glass Cockpit", "Night Watch", "Normal size", "Show tuner switches",
        "The UP and DN switches under each figure. The wheel over a figure still tunes it "
        "without them.",
        "Theme", "Today's bench", "Tuner switches",
    };
    int faults = 0;
    int catalogues = 0;
    for (const fs::path& path : catalogueFiles()) {
        std::string bytes;
        CHECK(readExact(path, bytes));
        const nlohmann::json j = nlohmann::json::parse(bytes, nullptr, false);
        if (j.is_discarded() || !j.contains("strings")) {
            CHECK(false);
            continue;
        }
        ++catalogues;
        const auto& strings = j["strings"];
        std::map<std::string, std::string> seen;  // translation -> the key that had it
        for (const char* key : kThemeKeys) {
            const auto it = strings.find(key);
            if (it == strings.end() || !it->is_string()) { continue; }
            const std::string v = it->get<std::string>();
            const auto [at, fresh] = seen.emplace(v, key);
            if (!fresh) {
                std::printf("      %s: \"%s\" and \"%s\" both translate to \"%s\"\n",
                            path.stem().string().c_str(), at->second.c_str(), key, v.c_str());
                ++faults;
            }
        }
    }
    CHECK(catalogues >= 30);
    CHECK(faults == 0);
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc >= 2 ? std::string(argv[1]) : findRoot();
    if (g_root.empty() || !fs::is_directory(fs::path(g_root) / "src")) {
        std::printf("cannot find the repository root (pass it as the first argument)\n");
        CHECK(false);
        return testSummary("test_i18n");
    }
    // The engine half adds catalogues of its own; the shipped half reads the
    // files directly, so the two cannot interfere.
    testEnglishPassthrough();
    testCatalogueLookup();
    testTrIdForms();
    testPointersSurviveSwitching();
    testResolve();
    testMalformed();
    testCatalogueFile();
    testFormatSpecs();
    testLocale();
    testExtractor();
    testFixedFormatScanner();
    testNoTranslatedFixedBuffers();
    testShippedCatalogues();
    testEmbeddedMatchesFiles();
    testCjkTemplateJoins();
    testThemeWordsDistinct();
    return testSummary("test_i18n");
}
