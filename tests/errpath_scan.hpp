// errpath_scan.hpp - THE ERROR-PATH SCANNER (the engine of the gate tests/test_error_paths.cpp).
//
// The owner's rule: nothing may fail in any install without him being told. This header reads
// SOURCE TEXT (no build configuration, no generated file, no link against the engine) and finds
// every place the program notices a failure:
//
//   warn      a diagWarnf( call
//   log       a diagLogf( call whose first literal carries failure wording (kFailureWords)
//   seterr    a setError( / setErrorOn( call
//   errassign an assignment to an error field (error, err, lastError_, sourceError_, *Error_ ...)
//   swallow   a catch handler whose body, once comments, diagLogf/diagWarnf calls and bare
//             `return;` / `continue;` are removed, is empty
//   ignored   a bare statement call, or a (void) cast, of a vendor-library call (Pa_*, libusb_*,
//             hackrf_*, airspy_*, airspyhf_*, rtlsdr_*, sdrplay_api_*, iio_*, FT_*, WinUsb_*)
//   stderr    a statement that writes to stderr (fprintf(stderr, ...), std::cerr <<)
//   plwarn    (profile "plugins" only) CASCADE_LOG_WARN / CASCADE_LOG_ERROR handed to the host log
//
// and requires each site to PASS one of three ways (see check()): a reporting call in a brace
// block that encloses the site; an allow-list entry with a reason; a PENDING:<group> entry whose
// count is pinned and may only go down.
//
// The key of a site has NO LINE NUMBER:  file::function::kind::stem[#n]  (stem = the first string
// literal of the site, lower-cased, digits -> #, printf conversions -> %, punctuation dropped,
// cut at 60 characters; a site with no literal uses its identifier skeleton; the second and later
// identical keys in one file get #2, #3 in file order). Moving code inside a file changes nothing.
//
// The same header is vendored, byte for byte, into tools/errpath-for-plugins/ for the plugins
// repository (test_error_paths checks the copy is identical).
//
// What the scanner CANNOT see (and says so): a reporter in a different branch of the same
// function than the site is not counted (the reporter must be in a block that ENCLOSES the site);
// a message built in a helper and logged from many places; a macro that wraps diagWarnf; code
// loaded at run time (.slint, scripts); a (void)-discarded result of the project's OWN calls (not a
// vendor prefix) - left out by name, not half-matched.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace errpath {

inline constexpr const char* kFailureWords[] = {
    "could not", "failed", "refused", "did not", "timed out", "lost", "abandon", "fault", "error",
    "unable", "cannot", "dropped", "stalled", "dead", "falling back",
};   // plus the upper-case word "NOT", matched case-sensitively

inline constexpr const char* kVendorPrefixes[] = {
    "Pa_", "libusb_", "hackrf_", "airspy_", "airspyhf_", "rtlsdr_", "sdrplay_api_", "iio_", "FT_", "WinUsb_",
};

struct Site {
    std::string file;       // path relative to the repository root, forward slashes ("src/core/x.cpp")
    std::string function;   // qualified enclosing function (class names kept, namespaces dropped)
    std::string kind;
    std::string stem;
    std::string key;        // file::function::kind::stem[#n]
    std::string snippet;    // up to 100 characters of the code from the site on, whitespace collapsed
    int line = 0;           // informational only: never part of a key, never read by the seed
    std::size_t pos = 0;
    bool reported = false;
    std::string reporter;   // the reporter that covers it
};

struct Options {
    std::string root;                          // repository root
    std::vector<std::string> dirs{"src"};      // directories (under root) to walk
    std::string profile = "engine";            // "engine" or "plugins"
    std::vector<std::string> reporters;        // names of the reporting calls
    int pin = -1;                              // the pinned PENDING count (-1: do not enforce)
};

struct AllowEntry {
    std::string key, verdict, reason, added;
    int lineNo = 0;
};

// ---------------------------------------------------------------------------------------------
// Lexing: comments, strings and characters blanked (newlines and offsets kept)
// ---------------------------------------------------------------------------------------------
struct Lit {
    std::size_t b = 0, e = 0;   // b: the opening quote, e: one past the closing quote
    std::string text;
};

struct Lexed {
    std::string code;      // comments and literal contents blanked
    std::string codeNoPP;  // the same with preprocessor lines blanked too (used for the braces)
    std::vector<Lit> lits;
};

inline bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
inline bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

inline Lexed lex(const std::string& s) {
    Lexed L;
    L.code = s;
    std::string& out = L.code;
    const std::size_t n = s.size();
    enum { Code, Line, Block } st = Code;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[i];
        const char nx = i + 1 < n ? s[i + 1] : '\0';
        if (st == Line) {
            if (c == '\n') {
                const bool cont = i > 0 && (s[i - 1] == '\\' || (s[i - 1] == '\r' && i > 1 && s[i - 2] == '\\'));
                if (!cont) { st = Code; }
            } else {
                out[i] = ' ';
            }
            continue;
        }
        if (st == Block) {
            if (c == '*' && nx == '/') { out[i] = ' '; out[i + 1] = ' '; ++i; st = Code; }
            else if (c != '\n') { out[i] = ' '; }
            continue;
        }
        if (c == '/' && nx == '/') { st = Line; out[i] = ' '; continue; }
        if (c == '/' && nx == '*') { st = Block; out[i] = ' '; out[i + 1] = ' '; ++i; continue; }
        if (c == '"') {
            // a raw string: R"delim( ... )delim" (with an optional u8 / u / U / L before the R)
            const bool raw = i >= 1 && s[i - 1] == 'R' &&
                             (i < 2 || !isIdentChar(s[i - 2]) || s[i - 2] == '8' || s[i - 2] == 'u' || s[i - 2] == 'U' || s[i - 2] == 'L');
            if (raw) {
                std::size_t d = i + 1;
                while (d < n && s[d] != '(' && d - i < 20) { ++d; }
                if (d < n && s[d] == '(') {
                    const std::string close = ")" + s.substr(i + 1, d - i - 1) + "\"";
                    const std::size_t e = s.find(close, d + 1);
                    if (e != std::string::npos) {
                        Lit lit;
                        lit.b = i;
                        lit.e = e + close.size();
                        lit.text = s.substr(d + 1, e - d - 1);
                        for (std::size_t k = i + 1; k + 1 < lit.e; ++k) { if (s[k] != '\n') { out[k] = ' '; } }
                        L.lits.push_back(lit);
                        i = lit.e - 1;
                        continue;
                    }
                }
            }
            std::size_t k = i + 1;
            while (k < n && s[k] != '"' && s[k] != '\n') { if (s[k] == '\\' && k + 1 < n) { ++k; } ++k; }
            Lit lit;
            lit.b = i;
            lit.e = k < n && s[k] == '"' ? k + 1 : k;
            lit.text = s.substr(i + 1, (k < n ? k : n) - i - 1);
            for (std::size_t q = i + 1; q < k && q < n; ++q) { out[q] = ' '; }
            L.lits.push_back(lit);
            i = lit.e > 0 ? lit.e - 1 : i;
            continue;
        }
        if (c == '\'') {
            if (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) { continue; }   // a digit separator
            std::size_t k = i + 1;
            while (k < n && s[k] != '\'' && s[k] != '\n') { if (s[k] == '\\' && k + 1 < n) { ++k; } ++k; }
            for (std::size_t q = i + 1; q < k && q < n; ++q) { out[q] = ' '; }
            i = k < n ? k : n - 1;
            continue;
        }
    }
    // preprocessor lines (a # first on its line, continued by a trailing backslash)
    L.codeNoPP = L.code;
    std::size_t i = 0;
    while (i < n) {
        std::size_t e = L.code.find('\n', i);
        if (e == std::string::npos) { e = n; }
        std::size_t f = i;
        while (f < e && (L.code[f] == ' ' || L.code[f] == '\t')) { ++f; }
        if (f < e && L.code[f] == '#') {
            while (true) {
                for (std::size_t q = i; q < e; ++q) { L.codeNoPP[q] = ' '; }
                std::size_t last = e;
                while (last > i && (L.code[last - 1] == '\r' || L.code[last - 1] == ' ')) { --last; }
                if (last > i && L.code[last - 1] == '\\' && e < n) {
                    i = e + 1;
                    e = L.code.find('\n', i);
                    if (e == std::string::npos) { e = n; }
                    continue;
                }
                break;
            }
        }
        i = e + 1;
    }
    return L;
}

// ---------------------------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------------------------
inline std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) { ++b; }
    while (e > b && isSpace(s[e - 1])) { --e; }
    return s.substr(b, e - b);
}

inline std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

inline bool hasWord(const std::string& h, const std::string& w) {
    std::size_t p = 0;
    while ((p = h.find(w, p)) != std::string::npos) {
        const bool lb = p == 0 || !isIdentChar(h[p - 1]);
        const bool rb = p + w.size() >= h.size() || !isIdentChar(h[p + w.size()]);
        if (lb && rb) { return true; }
        p += w.size();
    }
    return false;
}

inline std::string collapse(const std::string& s, std::size_t cut) {
    std::string out;
    bool sp = false;
    for (const char c : s) {
        if (isSpace(c)) { sp = true; continue; }
        if (sp && !out.empty()) { out.push_back(' '); }
        sp = false;
        out.push_back(c);
        if (out.size() >= cut) { break; }
    }
    return out;
}

// the stem of a string literal's text
inline std::string stemOfLiteral(const std::string& t) {
    std::string out;
    const std::size_t n = t.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char c = t[i];
        if (c == '%') {
            if (i + 1 < n && t[i + 1] == '%') { ++i; continue; }
            std::size_t k = i + 1;
            while (k < n && std::string("-+ #0123456789.*").find(t[k]) != std::string::npos) { ++k; }
            while (k < n && std::string("lhzjtLq").find(t[k]) != std::string::npos) { ++k; }
            if (k < n && std::string("diouxXeEfFgGaAcspn").find(t[k]) != std::string::npos) {
                out.push_back('%');
                i = k;
                continue;
            }
            continue;
        }
        if (c == '\\' && i + 1 < n) { ++i; if (t[i] == 'n' || t[i] == 't') { out.push_back(' '); } continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (out.empty() || out.back() != '#') { out.push_back('#'); }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) { out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); continue; }
        if (isSpace(c)) { out.push_back(' '); continue; }
        // punctuation dropped
    }
    std::string flat;
    bool sp = false;
    for (const char c : out) {
        if (c == ' ') { sp = true; continue; }
        if (sp && !flat.empty()) { flat.push_back(' '); }
        sp = false;
        flat.push_back(c);
    }
    if (flat.size() > 60) { flat.resize(60); }
    while (!flat.empty() && flat.back() == ' ') { flat.pop_back(); }
    return flat;
}

// the identifier skeleton of a code slice (a site with no string literal)
inline std::string skeletonOf(const std::string& code) {
    std::string out;
    std::size_t i = 0;
    while (i < code.size() && out.size() < 60) {
        if (isIdentStart(code[i])) {
            std::size_t j = i;
            while (j < code.size() && isIdentChar(code[j])) { ++j; }
            if (!out.empty()) { out.push_back(' '); }
            out += lower(code.substr(i, j - i));
            i = j;
        } else if (std::isdigit(static_cast<unsigned char>(code[i]))) {
            while (i < code.size() && (isIdentChar(code[i]) || code[i] == '.')) { ++i; }
            if (!out.empty()) { out.push_back(' '); }
            out.push_back('#');
        } else {
            ++i;
        }
    }
    if (out.size() > 60) { out.resize(60); }
    while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Blocks and enclosing functions
// ---------------------------------------------------------------------------------------------
enum class BK { Namespace, Class, Func, Nested };

struct Block {
    std::size_t open = 0, close = 0;
    int parent = -1;
    BK kind = BK::Nested;
    std::string name;
};

inline bool isContainer(BK k) { return k == BK::Namespace || k == BK::Class; }

// the name of the function (or variable) a header declares: the identifier chain before the first top-level (
inline std::string functionNameOf(const std::string& h) {
    const std::size_t op = h.find("operator");
    std::size_t paren = std::string::npos;
    int angle = 0;
    for (std::size_t i = 0; i < h.size(); ++i) {
        if (h[i] == '<') { ++angle; }
        else if (h[i] == '>' && angle > 0) { --angle; }
        else if (h[i] == '(' && angle == 0) {
            if (op != std::string::npos && i < op) { paren = i; break; }   // a parenthesis before 'operator': take it
            paren = i;
            break;
        }
    }
    if (op != std::string::npos && (paren == std::string::npos || paren > op) && hasWord(h, "operator")) {
        std::size_t k = op + 8;
        while (k < h.size() && isSpace(h[k])) { ++k; }
        std::size_t b = op;
        while (b > 0 && (isIdentChar(h[b - 1]) || h[b - 1] == ':')) { --b; }
        std::string sym;
        if (k < h.size() && h[k] == '(' && k + 1 < h.size() && h[k + 1] == ')') { sym = "()"; }
        else { while (k < h.size() && !isIdentChar(h[k]) && h[k] != '(' && !isSpace(h[k])) { sym.push_back(h[k]); ++k; } }
        return h.substr(b, op - b) + "operator" + sym;
    }
    if (paren == std::string::npos) { return "<block>"; }
    std::size_t e = paren;
    while (e > 0 && isSpace(h[e - 1])) { --e; }
    std::size_t b = e;
    while (b > 0) {
        const char c = h[b - 1];
        if (isIdentChar(c) || c == ':' || c == '~') { --b; continue; }
        if (c == '>') {   // skip a template argument list
            int d = 0;
            std::size_t q = b;
            while (q > 0) {
                --q;
                if (h[q] == '>') { ++d; }
                else if (h[q] == '<') { if (--d == 0) { break; } }
            }
            b = q;
            continue;
        }
        break;
    }
    std::string name = h.substr(b, e - b);
    // drop template argument lists from the name
    std::string clean;
    int d = 0;
    for (const char c : name) {
        if (c == '<') { ++d; continue; }
        if (c == '>') { if (d > 0) { --d; } continue; }
        if (d == 0) { clean.push_back(c); }
    }
    if (clean.empty()) { return "<block>"; }
    return clean;
}

inline std::string classNameOf(std::string h) {
    // remove a leading template<...>
    const std::size_t t = h.find("template");
    if (t != std::string::npos && hasWord(h, "template")) {
        std::size_t k = h.find('<', t);
        int d = 0;
        while (k != std::string::npos && k < h.size()) {
            if (h[k] == '<') { ++d; }
            else if (h[k] == '>') { if (--d == 0) { ++k; break; } }
            ++k;
        }
        if (k != std::string::npos && k <= h.size()) { h = h.substr(k); }
    }
    std::size_t kw = std::string::npos;
    for (const char* w : {"class", "struct", "union"}) {
        std::size_t p = 0;
        while ((p = h.find(w, p)) != std::string::npos) {
            const std::size_t len = std::string(w).size();
            if ((p == 0 || !isIdentChar(h[p - 1])) && (p + len >= h.size() || !isIdentChar(h[p + len]))) {
                if (kw == std::string::npos || p < kw) { kw = p + len; }
                break;
            }
            p += len;
        }
    }
    if (kw == std::string::npos) { return "<class>"; }
    std::string seg = h.substr(kw);
    // blank the contents of (...) and [[...]] and cut at the base clause
    std::string flat;
    int pd = 0;
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const char c = seg[i];
        if (c == '(') { ++pd; continue; }
        if (c == ')') { if (pd > 0) { --pd; } continue; }
        if (pd > 0) { continue; }
        if (c == ':' && !(i + 1 < seg.size() && seg[i + 1] == ':') && !(i > 0 && seg[i - 1] == ':')) { break; }
        flat.push_back(c);
    }
    std::string last;
    std::size_t i = 0;
    while (i < flat.size()) {
        if (isIdentStart(flat[i])) {
            std::size_t j = i;
            while (j < flat.size() && (isIdentChar(flat[j]) || flat[j] == ':')) { ++j; }
            const std::string w = flat.substr(i, j - i);
            if (w != "final" && w != "alignas" && w != "__declspec") { last = w; }
            i = j;
        } else {
            ++i;
        }
    }
    return last.empty() ? "<class>" : last;
}

struct FileScan {
    std::string rel;
    std::vector<Site> sites;
    bool unbalanced = false;
};

struct Scanner {
    const std::string& src;
    const Lexed& L;
    const Options& opt;
    std::vector<Block> blocks;
    std::vector<std::size_t> opens;
    std::vector<std::size_t> lineStarts;
    bool unbalanced = false;

    Scanner(const std::string& s, const Lexed& l, const Options& o) : src(s), L(l), opt(o) {}

    int lineOf(std::size_t pos) const {
        const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), pos);
        return static_cast<int>(it - lineStarts.begin());
    }

    void buildBlocks() {
        const std::string& c = L.codeNoPP;
        const std::size_t n = c.size();
        lineStarts.push_back(0);
        for (std::size_t i = 0; i < n; ++i) { if (src[i] == '\n') { lineStarts.push_back(i + 1); } }
        // matching braces
        std::vector<std::size_t> match(n, std::string::npos);
        {
            std::vector<std::size_t> stk;
            for (std::size_t i = 0; i < n; ++i) {
                if (c[i] == '{') { stk.push_back(i); }
                else if (c[i] == '}') {
                    if (stk.empty()) { unbalanced = true; continue; }
                    match[stk.back()] = i;
                    stk.pop_back();
                }
            }
            if (!stk.empty()) { unbalanced = true; }
        }
        std::vector<int> st;
        std::size_t hs = 0;
        auto atContainer = [&]() { return st.empty() || isContainer(blocks[static_cast<std::size_t>(st.back())].kind); };
        for (std::size_t i = 0; i < n; ++i) {
            const char ch = c[i];
            if (ch == ';') {
                if (atContainer()) { hs = i + 1; }
            } else if (ch == '}') {
                if (!st.empty()) {
                    blocks[static_cast<std::size_t>(st.back())].close = i;
                    st.pop_back();
                }
                if (atContainer()) { hs = i + 1; }
            } else if (ch == ':' && atContainer() && !(i + 1 < n && c[i + 1] == ':') && !(i > 0 && c[i - 1] == ':')) {
                const std::string h = trim(c.substr(hs, i - hs));
                if (h == "public" || h == "private" || h == "protected" || h == "signals" || h == "slots") { hs = i + 1; }
            } else if (ch == '{') {
                Block b;
                b.open = i;
                b.close = match[i] == std::string::npos ? n : match[i];
                b.parent = st.empty() ? -1 : st.back();
                if (!atContainer()) {
                    b.kind = BK::Nested;
                } else {
                    const std::string h = trim(c.substr(hs, i - hs));
                    classify(h, b);
                    if (b.name == "\x01skip") {   // a member brace-initialiser: not a block
                        i = match[i] == std::string::npos ? n : match[i];
                        continue;
                    }
                }
                blocks.push_back(b);
                opens.push_back(i);
                st.push_back(static_cast<int>(blocks.size()) - 1);
                hs = i + 1;
            }
        }
        for (const int idx : st) { blocks[static_cast<std::size_t>(idx)].close = n; }
    }

    static void classify(const std::string& h, Block& b) {
        if (h.empty()) { b.kind = BK::Func; b.name = "<block>"; return; }
        {   // a brace inside an unclosed ( or [ : a default argument  f(int x = {})  or an initialiser list in a call
            int pd = 0;
            for (const char ch : h) { if (ch == '(' || ch == '[') { ++pd; } else if (ch == ')' || ch == ']') { --pd; } }
            if (pd > 0) { b.name = "skip"; return; }
        }
        if (hasWord(h, "namespace") || (h.rfind("extern", 0) == 0 && h.find('(') == std::string::npos)) {
            b.kind = BK::Namespace;
            return;
        }
        // a member brace-initialiser in a constructor's initialiser list:  Foo::Foo() : a_(1), b_{2} {
        {
            const std::size_t rp = h.rfind(')');
            if (rp != std::string::npos && (isIdentChar(h.back()) || h.back() == '>')) {
                int pd = 0;
                bool colon = false;
                for (std::size_t i = 0; i < h.size(); ++i) {
                    if (h[i] == '(') { ++pd; }
                    else if (h[i] == ')') { --pd; }
                    else if (h[i] == ':' && pd == 0 && i > 0 && h[i - 1] != ':' && !(i + 1 < h.size() && h[i + 1] == ':')) { colon = true; }
                }
                if (colon && !hasWord(h, "enum")) { b.name = "\x01skip"; return; }
            }
        }
        if (hasWord(h, "enum")) { b.kind = BK::Func; b.name = "<enum>"; return; }
        const bool classKw = hasWord(h, "class") || hasWord(h, "struct") || hasWord(h, "union");
        if (classKw && h.back() != ')' && !hasWord(h, "operator")) {
            b.kind = BK::Class;
            b.name = classNameOf(h);
            return;
        }
        // a top-level '=' (data initialiser, lambda assigned to a variable)
        {
            int pd = 0;
            for (std::size_t i = 0; i < h.size(); ++i) {
                if (h[i] == '(' || h[i] == '[') { ++pd; }
                else if (h[i] == ')' || h[i] == ']') { --pd; }
                else if (h[i] == '=' && pd == 0 && !hasWord(h, "operator")) {
                    const char p = i > 0 ? h[i - 1] : ' ';
                    const char n2 = i + 1 < h.size() ? h[i + 1] : ' ';
                    if (p != '=' && p != '!' && p != '<' && p != '>' && n2 != '=') {
                        std::size_t e = i;
                        while (e > 0 && isSpace(h[e - 1])) { --e; }
                        if (e > 0 && h[e - 1] == ']') { int d = 0; while (e > 0) { --e; if (h[e] == ']') { ++d; } else if (h[e] == '[') { if (--d == 0) { break; } } } while (e > 0 && isSpace(h[e - 1])) { --e; } }
                        std::size_t s = e;
                        while (s > 0 && (isIdentChar(h[s - 1]) || h[s - 1] == ':')) { --s; }
                        b.kind = BK::Func;
                        b.name = s < e ? h.substr(s, e - s) : "<data>";
                        return;
                    }
                }
            }
        }
        if (h.find('(') == std::string::npos) {
            b.kind = BK::Func;
            std::size_t e = h.size();
            while (e > 0 && !isIdentChar(h[e - 1])) { --e; }
            std::size_t s = e;
            while (s > 0 && (isIdentChar(h[s - 1]) || h[s - 1] == ':')) { --s; }
            b.name = s < e ? h.substr(s, e - s) : "<data>";
            return;
        }
        b.kind = BK::Func;
        b.name = functionNameOf(h);
    }

    int innermost(std::size_t pos) const {
        auto it = std::upper_bound(opens.begin(), opens.end(), pos);
        int idx = static_cast<int>(it - opens.begin()) - 1;
        while (idx >= 0 && blocks[static_cast<std::size_t>(idx)].close <= pos) { idx = blocks[static_cast<std::size_t>(idx)].parent; }
        return idx;
    }

    // the outermost non-container block around pos, or -1
    int outermostFunc(int inner) const {
        int idx = inner;
        int best = -1;
        while (idx >= 0) {
            if (!isContainer(blocks[static_cast<std::size_t>(idx)].kind)) { best = idx; }
            idx = blocks[static_cast<std::size_t>(idx)].parent;
        }
        return best;
    }

    std::string functionName(int inner) const {
        const int f = outermostFunc(inner);
        std::vector<std::string> classes;
        int idx = f >= 0 ? blocks[static_cast<std::size_t>(f)].parent : inner;
        while (idx >= 0) {
            const Block& b = blocks[static_cast<std::size_t>(idx)];
            if (b.kind == BK::Class) { classes.push_back(b.name); }
            idx = b.parent;
        }
        std::reverse(classes.begin(), classes.end());
        std::string q;
        for (const auto& cn : classes) { q += cn + "::"; }
        if (f < 0) { return classes.empty() ? "<file>" : "<scope " + q.substr(0, q.size() - 2) + ">"; }
        const std::string& nm = blocks[static_cast<std::size_t>(f)].name;
        if (nm.find("::") != std::string::npos) { return nm; }
        return q + nm;
    }

    // ---- literal and statement helpers --------------------------------------------------
    // the first string literal that begins in [a, b), adjacent literals joined
    std::string firstLiteral(std::size_t a, std::size_t b, bool* found = nullptr) const {
        const auto it = std::lower_bound(L.lits.begin(), L.lits.end(), a, [](const Lit& l, std::size_t v) { return l.b < v; });
        if (it == L.lits.end() || it->b >= b) { if (found) { *found = false; } return {}; }
        if (found) { *found = true; }
        std::string out = it->text;
        std::size_t prevEnd = it->e;
        for (auto k = it + 1; k != L.lits.end() && k->b < b; ++k) {
            bool gap = true;
            for (std::size_t q = prevEnd; q < k->b; ++q) { if (!isSpace(L.code[q])) { gap = false; break; } }
            if (!gap) { break; }
            out += k->text;
            prevEnd = k->e;
        }
        return out;
    }

    // end of the statement beginning at `from`: the ; at depth 0 (or the closing bracket that ends the enclosing group)
    std::size_t statementEnd(std::size_t from) const {
        const std::string& c = L.code;
        int d = 0;
        for (std::size_t i = from; i < c.size(); ++i) {
            const char ch = c[i];
            if (ch == '(' || ch == '[' || ch == '{') { ++d; }
            else if (ch == ')' || ch == ']' || ch == '}') { if (--d < 0) { return i; } }
            else if (ch == ';' && d == 0) { return i; }
        }
        return c.size();
    }

    std::size_t matchParen(std::size_t open) const {
        const std::string& c = L.code;
        int d = 0;
        for (std::size_t i = open; i < c.size(); ++i) {
            if (c[i] == '(') { ++d; }
            else if (c[i] == ')') { if (--d == 0) { return i; } }
        }
        return c.size();
    }

    // is the identifier at [i, j) a declaration of a function of that name (rather than a call)?
    bool isDeclaration(std::size_t i) const {
        const std::string& c = L.code;
        std::size_t k = i;
        // skip a qualifier chain  A::B::
        while (true) {
            std::size_t q = k;
            while (q > 0 && isSpace(c[q - 1])) { --q; }
            if (q >= 2 && c[q - 1] == ':' && c[q - 2] == ':') {
                q -= 2;
                while (q > 0 && isSpace(c[q - 1])) { --q; }
                std::size_t s = q;
                while (s > 0 && isIdentChar(c[s - 1])) { --s; }
                if (s == q) { break; }
                k = s;
                continue;
            }
            break;
        }
        std::size_t q = k;
        while (q > 0 && isSpace(c[q - 1])) { --q; }
        if (q == 0) { return false; }
        const char p = c[q - 1];
        if (p == '.') { return false; }
        if (p == '>' && q >= 2 && c[q - 2] == '-') { return false; }
        if (p == '>') { return true; }
        if (p == '~') { return true; }
        if (p == '&' || p == '*') { return !(q >= 2 && c[q - 2] == p); }
        if (isIdentChar(p)) {
            std::size_t s = q;
            while (s > 0 && isIdentChar(c[s - 1])) { --s; }
            const std::string w = c.substr(s, q - s);
            static const std::set<std::string> kCallWords{"return", "else", "do", "case", "throw", "co_return", "co_yield", "new", "delete", "goto", "sizeof", "not", "and", "or"};
            return kCallWords.count(w) == 0;
        }
        return false;
    }

    // start of a member chain  a.b->c::name  ending at i
    std::size_t chainStart(std::size_t i) const {
        const std::string& c = L.code;
        std::size_t k = i;
        while (true) {
            std::size_t q = k;
            while (q > 0 && isSpace(c[q - 1])) { --q; }
            std::size_t sep = 0;
            if (q >= 2 && c[q - 1] == ':' && c[q - 2] == ':') { sep = 2; }
            else if (q >= 2 && c[q - 1] == '>' && c[q - 2] == '-') { sep = 2; }
            else if (q >= 1 && c[q - 1] == '.') { sep = 1; }
            if (!sep) { break; }
            q -= sep;
            while (q > 0 && isSpace(c[q - 1])) { --q; }
            // the operand: identifier, or a bracketed/parenthesised tail like x[1] or f()
            std::size_t e = q;
            if (e > 0 && (c[e - 1] == ']' || c[e - 1] == ')')) {
                const char closeCh = c[e - 1];
                const char openCh = closeCh == ']' ? '[' : '(';
                int d = 0;
                while (e > 0) { --e; if (c[e] == closeCh) { ++d; } else if (c[e] == openCh) { if (--d == 0) { break; } } }
                while (e > 0 && isSpace(c[e - 1])) { --e; }
            }
            std::size_t s = e;
            while (s > 0 && isIdentChar(c[s - 1])) { --s; }
            if (s == e) { break; }
            k = s;
        }
        return k;
    }

    // is the statement at `start` (a chain start) really a bare statement: previous significant char is ; { } or the
    // closing paren of if/for/while, or else/do - or a (void) cast?
    bool isBareStatement(std::size_t start, bool* voidCast) const {
        const std::string& c = L.code;
        std::size_t q = start;
        while (q > 0 && isSpace(c[q - 1])) { --q; }
        *voidCast = false;
        if (q >= 2 && c[q - 1] == ':' && c[q - 2] == ':') {   // a global qualifier  ::WinUsb_Free(...)
            q -= 2;
            while (q > 0 && isSpace(c[q - 1])) { --q; }
        }
        if (q == 0) { return true; }
        const char p = c[q - 1];
        if (p == ';' || p == '{' || p == '}' || p == ':') { return true; }
        if (p == ')') {
            // (void)  or  if (...) / for (...) / while (...)
            std::size_t o = q - 1;
            int d = 0;
            while (true) { if (c[o] == ')') { ++d; } else if (c[o] == '(') { if (--d == 0) { break; } } if (o == 0) { return false; } --o; }
            const std::string inner = trim(c.substr(o + 1, q - 1 - o - 1));
            if (inner == "void") { *voidCast = true; return true; }
            std::size_t e = o;
            while (e > 0 && isSpace(c[e - 1])) { --e; }
            std::size_t s = e;
            while (s > 0 && isIdentChar(c[s - 1])) { --s; }
            const std::string w = c.substr(s, e - s);
            return w == "if" || w == "for" || w == "while" || w == "switch";
        }
        if (isIdentChar(p)) {
            std::size_t s = q;
            while (s > 0 && isIdentChar(c[s - 1])) { --s; }
            const std::string w = c.substr(s, q - s);
            return w == "else" || w == "do";
        }
        return false;
    }

    // ---- sites -----------------------------------------------------------------------------
    std::vector<Site> sites;

    void add(const std::string& rel, const char* kind, std::size_t pos, std::size_t startForSnippet, const std::string& stem) {
        Site s;
        s.file = rel;
        s.kind = kind;
        s.pos = pos;
        s.line = lineOf(pos);
        s.stem = stem;
        s.snippet = collapse(src.substr(startForSnippet, 400), 100);
        const int inner = innermost(pos);
        s.function = functionName(inner);
        sites.push_back(std::move(s));
    }

    static bool failureWording(const std::string& lit) {
        const std::string lo = lower(lit);
        for (const char* w : kFailureWords) { if (lo.find(w) != std::string::npos) { return true; } }
        return lit.find("NOT") != std::string::npos;
    }

    static bool errorFieldName(const std::string& id) {
        if (id.empty()) { return false; }
        std::string b = id;
        if (b.back() == '_') { b.pop_back(); }
        if (b.empty()) { return false; }
        if (b.size() >= 2 && std::islower(static_cast<unsigned char>(b[0])) && std::isupper(static_cast<unsigned char>(b[1]))) {
            // a Hungarian prefix (hStdError, fAbortOnError: Win32 structure fields) - not ours; except real camel names
            // like lastError / sourceError, whose second letter is lower case: those pass the test above
            return false;
        }
        if (b.size() >= 2 && b[0] == 'k' && std::isupper(static_cast<unsigned char>(b[1]))) { return false; }   // a constant
        if (b == "error" || b == "err" || b == "errMsg" || b == "errorMsg" || b == "errorText" || b == "errText" || b == "lastErr" ||
            b == "lastError" || b == "faultMsg" || b == "win32Error") { return true; }
        auto endsWith = [&](const char* suf) { const std::string s(suf); return b.size() > s.size() && b.compare(b.size() - s.size(), s.size(), s) == 0; };
        return endsWith("Error");   // (NOT *Err: bestErr, haveErr are measurements, openErr/startErr are left out by name - see the record)
    }

    void scan(const std::string& rel) {
        const std::string& c = L.code;
        const std::size_t n = c.size();
        const bool plugins = opt.profile == "plugins";
        for (std::size_t i = 0; i < n;) {
            if (isIdentStart(c[i]) && (i == 0 || !isIdentChar(c[i - 1]))) {
                std::size_t j = i;
                while (j < n && isIdentChar(c[j])) { ++j; }
                const std::string id = c.substr(i, j - i);
                std::size_t k = j;
                while (k < n && isSpace(c[k])) { ++k; }
                const bool call = k < n && c[k] == '(';
                if (call && (id == "diagWarnf" || id == "diagLogf" || id == "setError" || id == "setErrorOn")) {
                    if (!isDeclaration(i)) {
                        const std::size_t e = matchParen(k);
                        bool found = false;
                        const std::string lit = firstLiteral(k, e, &found);
                        const std::size_t cs = chainStart(i);
                        if (id == "diagWarnf") {
                            add(rel, "warn", i, cs, found ? stemOfLiteral(lit) : skeletonOf(c.substr(k, e - k)));
                        } else if (id == "diagLogf") {
                            if (found && failureWording(lit)) { add(rel, "log", i, cs, stemOfLiteral(lit)); }
                        } else {
                            add(rel, "seterr", i, cs, found ? stemOfLiteral(lit) : skeletonOf(c.substr(k, e - k)));
                        }
                    }
                } else if (call && id == "catch") {
                    const std::size_t e = matchParen(k);
                    std::size_t b = e + 1;
                    while (b < n && isSpace(c[b])) { ++b; }
                    if (b < n && c[b] == '{') {
                        std::size_t close = std::string::npos;
                        int d = 0;
                        for (std::size_t q = b; q < n; ++q) { if (c[q] == '{') { ++d; } else if (c[q] == '}') { if (--d == 0) { close = q; break; } } }
                        if (close != std::string::npos && emptyHandler(c.substr(b + 1, close - b - 1))) {
                            add(rel, "swallow", i, i, "catch " + skeletonOf(c.substr(k + 1, e - k - 1)));
                        }
                    }
                } else if (call) {
                    for (const char* pre : kVendorPrefixes) {
                        const std::string p(pre);
                        if (id.size() > p.size() && id.compare(0, p.size(), p) == 0) {
                            const std::size_t cs = chainStart(i);
                            bool voidCast = false;
                            if (!isDeclaration(i) && isBareStatement(cs, &voidCast)) {
                                const std::size_t e = matchParen(k);
                                // a call whose value is used (x = f(), return f()) never reaches here: bare statements only
                                add(rel, "ignored", i, cs, skeletonOf(c.substr(cs, e - cs)));
                            }
                            break;
                        }
                    }
                } else if (id == "stderr" || id == "cerr") {
                    // one site per statement: anchor at the statement start
                    std::size_t b = i;
                    while (b > 0 && c[b - 1] != ';' && c[b - 1] != '{' && c[b - 1] != '}') { --b; }
                    while (b < i && isSpace(c[b])) { ++b; }
                    const std::size_t e = statementEnd(i);
                    const bool stream = id == "cerr";
                    const bool viaCall = id == "stderr";
                    bool ok = false;
                    if (viaCall) {
                        const std::string head = c.substr(b, i - b);
                        ok = head.find("fprintf") != std::string::npos || head.find("fputs") != std::string::npos ||
                             head.find("fwrite") != std::string::npos || head.find("fputc") != std::string::npos ||
                             head.find("vfprintf") != std::string::npos;
                    } else if (stream) {
                        std::size_t a = k;
                        ok = a + 1 < n && c[a] == '<' && c[a + 1] == '<';
                    }
                    if (ok) {
                        bool found = false;
                        const std::string lit = firstLiteral(b, e, &found);
                        add(rel, "stderr", i, b, found ? stemOfLiteral(lit) : skeletonOf(c.substr(b, e - b)));
                    }
                } else if (plugins && (id == "CASCADE_LOG_WARN" || id == "CASCADE_LOG_ERROR")) {
                    std::size_t b = i;
                    while (b > 0 && c[b - 1] != ';' && c[b - 1] != '{' && c[b - 1] != '}') { --b; }
                    while (b < i && isSpace(c[b])) { ++b; }
                    const std::size_t e = statementEnd(i);
                    bool found = false;
                    const std::string lit = firstLiteral(i, e, &found);
                    add(rel, "plwarn", i, i, id == "CASCADE_LOG_WARN" ? "warn " + (found ? stemOfLiteral(lit) : skeletonOf(c.substr(i, e - i)))
                                                                         : "error " + (found ? stemOfLiteral(lit) : skeletonOf(c.substr(i, e - i))));
                } else if (!call && errorFieldName(id)) {
                    errAssign(rel, i, j);
                } else if (call && errorFieldName(id)) {
                    // a call to a function named like an error field is not an assignment
                } else {
                    // nothing
                }
                i = j;
                continue;
            }
            ++i;
        }
    }

    // an assignment to an error field whose identifier is at [i, j)
    void errAssign(const std::string& rel, std::size_t i, std::size_t j) {
        const std::string& c = L.code;
        const std::size_t n = c.size();
        std::size_t k = j;
        while (k < n && isSpace(c[k])) { ++k; }
        if (k < n && c[k] == '[') {   // patchRadioError_[id] = ...
            int d = 0;
            while (k < n) { if (c[k] == '[') { ++d; } else if (c[k] == ']') { if (--d == 0) { ++k; break; } } ++k; }
            while (k < n && isSpace(c[k])) { ++k; }
        }
        if (k >= n) { return; }
        bool plusEq = false;
        if (c[k] == '+' && k + 1 < n && c[k + 1] == '=') { plusEq = true; k += 2; }
        else if (c[k] == '=' && !(k + 1 < n && c[k + 1] == '=')) { k += 1; }
        else { return; }
        // a compound  err += dy  with no text in it is arithmetic (an error TERM), not a message
        if (plusEq) {
            bool anyLit = false;
            const std::size_t e0 = statementEnd(k);
            firstLiteral(k, e0, &anyLit);
            if (!anyLit) { return; }
        }
        // preceding char: not part of a comparison / compound operator  (<= >= != == |= ...)
        const std::size_t e = statementEnd(k);
        std::string rhs = trim(src.substr(k, e - k));
        // clearing is not a failure: "", {}, std::string(), nullptr, false, 0, true
        const std::string rl = rhs;
        if (rl.empty() || rl == "\"\"" || rl == "{}" || rl == "std::string()" || rl == "std::string{}" || rl == "nullptr" || rl == "false" ||
            rl == "true" || rl == "0" || rl == "''" || rl == "{ }" || rl == "std::string{ }") { return; }
        // a character buffer cleared or declared:  err[0] = 0;   char err[160] = {0};
        if (rl[0] == '\'' || rl[0] == '{') { return; }
        // outside any function body (a class's default member initialiser, an enum constant, a namespace-scope constant)
        // an assignment is a declaration, not a failure being noticed
        {
            const int f = outermostFunc(innermost(i));
            if (f < 0 || blocks[static_cast<std::size_t>(f)].name == "<enum>") { return; }
        }
        const std::size_t cs = chainStart(i);
        bool found = false;
        const std::string lit = firstLiteral(k, e, &found);
        const std::string stem = found ? stemOfLiteral(lit) : skeletonOf(c.substr(k, e - k));
        add(rel, "errassign", i, cs, stem);
    }

    static bool emptyHandler(std::string body) {
        // remove diagLogf( ... ); / diagWarnf( ... ); statements, then bare return; continue;
        auto strip = [&](const std::string& name) {
            std::size_t p = 0;
            while ((p = body.find(name, p)) != std::string::npos) {
                if (p > 0 && isIdentChar(body[p - 1])) { p += name.size(); continue; }
                std::size_t k = p + name.size();
                while (k < body.size() && isSpace(body[k])) { ++k; }
                if (k >= body.size() || body[k] != '(') { p += name.size(); continue; }
                int d = 0;
                std::size_t e = k;
                for (; e < body.size(); ++e) { if (body[e] == '(') { ++d; } else if (body[e] == ')') { if (--d == 0) { break; } } }
                while (e < body.size() && body[e] != ';' ) { ++e; }
                body.erase(p, (e < body.size() ? e + 1 : body.size()) - p);
            }
        };
        strip("diagLogf");
        strip("diagWarnf");
        for (const char* w : {"return", "continue"}) {
            std::size_t p = 0;
            while ((p = body.find(w, p)) != std::string::npos) {
                const std::size_t len = std::string(w).size();
                std::size_t k = p + len;
                while (k < body.size() && isSpace(body[k])) { ++k; }
                const bool lb = p == 0 || !isIdentChar(body[p - 1]);
                if (lb && k < body.size() && body[k] == ';') { body.erase(p, k + 1 - p); } else { p += len; }
            }
        }
        return trim(body).empty();
    }
};

// ---------------------------------------------------------------------------------------------
// Walking the tree
// ---------------------------------------------------------------------------------------------
inline std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool isSourceName(const std::string& name) {
    static const char* const kExt[] = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".c", ".hh", ".inl", ".mm"};
    for (const char* e : kExt) {
        const std::string x(e);
        if (name.size() > x.size() && name.compare(name.size() - x.size(), x.size(), x) == 0) { return true; }
    }
    return false;
}

inline bool excludedPath(const std::string& rel) {
    // tests and probes are not product code
    if (rel.find("/tests/") != std::string::npos || rel.find("/test/") != std::string::npos || rel.find("/probe/") != std::string::npos || rel.rfind("tests/", 0) == 0) { return true; }
    // vendored code (the plugins repository carries fdk-aac and friends under src/<plugin>/third_party/)
    if (rel.find("/third_party/") != std::string::npos || rel.find("/thirdparty/") != std::string::npos) { return true; }
    const std::size_t sl = rel.rfind('/');
    const std::string base = sl == std::string::npos ? rel : rel.substr(sl + 1);
    return base.rfind("test_", 0) == 0;
}

inline FileScan scanText(const std::string& rel, const std::string& text, const Options& opt, const std::vector<std::string>& reporterNames) {
    FileScan fs;
    fs.rel = rel;
    const Lexed L = lex(text);
    Scanner sc(text, L, opt);
    sc.buildBlocks();
    sc.scan(rel);
    fs.unbalanced = sc.unbalanced;
    // reporters: positions of calls to the reporting functions
    std::vector<std::size_t> reps;
    std::vector<std::string> repNames;
    {
        const std::string& c = L.code;
        for (std::size_t i = 0; i < c.size();) {
            if (isIdentStart(c[i]) && (i == 0 || !isIdentChar(c[i - 1]))) {
                std::size_t j = i;
                while (j < c.size() && isIdentChar(c[j])) { ++j; }
                const std::string id = c.substr(i, j - i);
                std::size_t k = j;
                while (k < c.size() && isSpace(c[k])) { ++k; }
                if (std::find(reporterNames.begin(), reporterNames.end(), id) != reporterNames.end() && (k < c.size() && (c[k] == '(' || (opt.profile == "plugins" && id.rfind("FOXPLUGIN_", 0) == 0)))) {
                    reps.push_back(i);
                    repNames.push_back(id);
                }
                i = j;
            } else {
                ++i;
            }
        }
    }
    std::vector<int> repBlock;
    for (const std::size_t p : reps) { repBlock.push_back(sc.innermost(p)); }
    // order and key the sites
    std::stable_sort(sc.sites.begin(), sc.sites.end(), [](const Site& a, const Site& b) { return a.pos < b.pos; });
    std::map<std::string, int> seen;
    for (Site& s : sc.sites) {
        // the reporter rule: a reporting call whose innermost block ENCLOSES the site (or is the site's own block)
        if (s.kind != "swallow") {
            const int inner = sc.innermost(s.pos);
            std::set<int> anc;
            for (int b = inner; b >= 0; b = sc.blocks[static_cast<std::size_t>(b)].parent) { anc.insert(b); }
            anc.insert(-1);
            for (std::size_t r = 0; r < reps.size(); ++r) {
                if (anc.count(repBlock[r]) != 0) {
                    // at file scope (no block) a reporter only counts inside the same statement-free region: refuse
                    if (repBlock[r] == -1) { continue; }
                    s.reported = true;
                    s.reporter = repNames[r];
                    break;
                }
            }
        }
        const std::string base = s.file + "::" + s.function + "::" + s.kind + "::" + s.stem;
        const int nth = ++seen[base];
        s.key = nth == 1 ? base : base + "#" + std::to_string(nth);
        if (nth > 1) { s.stem += "#" + std::to_string(nth); }
        fs.sites.push_back(s);
    }
    return fs;
}

inline std::vector<std::string> listFiles(const Options& opt) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::string rootStr = fs::path(opt.root).generic_string();
    if (!rootStr.empty() && rootStr.back() != '/') { rootStr.push_back('/'); }
    for (const std::string& d : opt.dirs) {
        const fs::path base = fs::path(opt.root) / d;
        std::error_code ec;
        if (!fs::exists(base, ec)) { continue; }
        for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) { break; }
            if (!it->is_regular_file(ec)) { continue; }
            const std::string name = it->path().filename().string();
            if (!isSourceName(name)) { continue; }
            std::string rel = it->path().generic_string().substr(rootStr.size());
            if (excludedPath(rel)) { continue; }
            out.push_back(rel);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

struct Scan {
    std::vector<Site> sites;
    std::vector<std::string> unbalancedFiles;
    int files = 0;
};

inline Scan scanTree(const Options& opt) {
    Scan sc;
    for (const std::string& rel : listFiles(opt)) {
        const std::string text = readFile(std::filesystem::path(opt.root) / rel);
        FileScan f = scanText(rel, text, opt, opt.reporters);
        ++sc.files;
        if (f.unbalanced) { sc.unbalancedFiles.push_back(rel); }
        for (Site& s : f.sites) { sc.sites.push_back(std::move(s)); }
    }
    return sc;
}

// ---------------------------------------------------------------------------------------------
// Allow-list and reporters files
// ---------------------------------------------------------------------------------------------
inline std::vector<std::string> readReporters(const std::string& path) {
    std::vector<std::string> out;
    std::istringstream in(readFile(path));
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') { continue; }
        out.push_back(line);
    }
    return out;
}

inline std::vector<AllowEntry> parseAllow(const std::string& text, std::vector<std::string>* problems) {
    std::vector<AllowEntry> out;
    std::istringstream in(text);
    std::string line;
    int no = 0;
    while (std::getline(in, line)) {
        ++no;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (trim(line).empty() || line[0] == '#') { continue; }
        std::vector<std::string> f;
        std::size_t b = 0;
        while (true) {
            const std::size_t t = line.find('\t', b);
            if (t == std::string::npos) { f.push_back(line.substr(b)); break; }
            f.push_back(line.substr(b, t - b));
            b = t + 1;
        }
        if (f.size() < 3 || f[0].empty() || f[1].empty()) {
            if (problems) { problems->push_back("allow list line " + std::to_string(no) + " is not 'key<TAB>verdict<TAB>reason[<TAB>added]'"); }
            continue;
        }
        AllowEntry e;
        e.key = f[0];
        e.verdict = f[1];
        e.reason = f[2];
        e.added = f.size() > 3 ? f[3] : "";
        e.lineNo = no;
        out.push_back(e);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------------------------
struct Result {
    int sites = 0, reported = 0, allowed = 0, pending = 0;
    std::map<std::string, int> byKind;
    std::map<std::string, int> pendingByGroup;
    std::vector<std::string> unresolved;   // one message per site that passes no way
    std::vector<std::string> stale;        // allow entries with no site
    std::vector<std::string> problems;     // malformed lines, bad reasons, dangling DETAIL-OF, pending that now report, pin
    bool ok() const { return unresolved.empty() && stale.empty() && problems.empty(); }
};

inline bool validReason(const std::string& r) {
    static const std::set<std::string> kCodes{"info", "user-action", "test-seam", "ui-text", "protocol-parse", "teardown", "validation", "reporter-itself"};
    return kCodes.count(r) != 0 || trim(r).size() >= 12;
}

inline Result check(const std::vector<Site>& sites, const std::vector<AllowEntry>& allow, const Options& opt) {
    Result r;
    std::map<std::string, const AllowEntry*> byKey;
    for (const AllowEntry& e : allow) {
        if (byKey.count(e.key) != 0) { r.problems.push_back("allow list: the key appears twice (line " + std::to_string(e.lineNo) + "): " + e.key); continue; }
        byKey[e.key] = &e;
    }
    std::map<std::string, const Site*> siteByKey;
    for (const Site& s : sites) { siteByKey[s.key] = &s; }
    std::set<std::string> used;
    r.sites = static_cast<int>(sites.size());
    // keys without the file part, of entries nothing matched (for the move hint)
    auto tail = [](const std::string& k) { const std::size_t p = k.find("::"); return p == std::string::npos ? k : k.substr(p + 2); };
    std::map<std::string, std::string> staleTail;
    for (const AllowEntry& e : allow) { if (siteByKey.count(e.key) == 0) { staleTail[tail(e.key)] = e.key; } }
    for (const Site& s : sites) {
        ++r.byKind[s.kind];
        const auto it = byKey.find(s.key);
        if (it != byKey.end()) { used.insert(s.key); }
        if (it != byKey.end() && it->second->verdict.rfind("PENDING:", 0) == 0) {
            if (s.reported) {
                r.problems.push_back("STALE PENDING (the site now reports through " + s.reporter + "): delete this line from the allow list AND lower the pin by one: " + s.key);
            }
            ++r.pending;
            ++r.pendingByGroup[it->second->verdict.substr(8)];
            continue;
        }
        if (s.reported) { ++r.reported; continue; }
        if (it == byKey.end()) {
            std::string msg = "UNREPORTED ERROR PATH: " + s.file + ", function " + s.function + ", kind " + s.kind + ": " + s.snippet +
                              "\n    The author must either report it (call noteRecovered / raiseRecovered / noteRadioFail / noteSoundFail or another reporter from tests/errpath_reporters.txt in this function) or"
                              " add this line to tests/errpath_allow.tsv with the reason it need not be reported:\n    " + s.key + "\tALLOW\t<reason, at least 12 characters>";
            const auto st = staleTail.find(tail(s.key));
            if (st != staleTail.end()) {
                msg += "\n    HINT: the allow list has an entry for the same function, kind and text under a different file (" + st->second.substr(0, st->second.find("::")) +
                       "): the function moved; edit that entry's file part instead of adding a new line.";
            }
            r.unresolved.push_back(msg);
            continue;
        }
        const AllowEntry& e = *it->second;
        if (e.verdict == "ALLOW") {
            if (!validReason(e.reason)) { r.problems.push_back("allow list line " + std::to_string(e.lineNo) + ": the reason must be a code (info, user-action, test-seam, ui-text, protocol-parse, teardown, validation, reporter-itself) or at least 12 characters: " + e.key); }
            ++r.allowed;
        } else if (e.verdict.rfind("DETAIL-OF:", 0) == 0) {
            const std::string target = e.verdict.substr(10);
            const auto tt = siteByKey.find(target);
            const auto ta = byKey.find(target);
            const bool ok = tt != siteByKey.end() && (tt->second->reported || (ta != byKey.end() && ta->second->verdict.rfind("PENDING:", 0) == 0));
            if (!ok) { r.problems.push_back("allow list line " + std::to_string(e.lineNo) + ": DETAIL-OF names a site that does not exist or is neither reported nor PENDING: " + target); }
            ++r.allowed;
        } else {
            r.problems.push_back("allow list line " + std::to_string(e.lineNo) + ": unknown verdict '" + e.verdict + "' (ALLOW, DETAIL-OF:<key> or PENDING:<group>)");
        }
    }
    for (const AllowEntry& e : allow) {
        if (siteByKey.count(e.key) == 0) {
            r.stale.push_back("STALE ALLOW ENTRY (no such site in the source; delete the line, or fix its file/function/text if the code moved): line " + std::to_string(e.lineNo) + ": " + e.key);
        }
    }
    if (opt.pin >= 0) {
        if (r.pending > opt.pin) {
            r.problems.push_back("PENDING count rose: " + std::to_string(r.pending) + " entries, the pin is " + std::to_string(opt.pin) +
                                 ". A new unreported site may not be parked as PENDING: report it, or allow it with a reason.");
        } else if (r.pending < opt.pin) {
            r.problems.push_back("PENDING count fell to " + std::to_string(r.pending) + " (pin " + std::to_string(opt.pin) + "): lower the pin to " + std::to_string(r.pending) +
                                 " in the test so the improvement cannot be lost.");
        }
    }
    return r;
}

inline void printResult(const Result& r, std::FILE* out) {
    std::fprintf(out, "error-path gate: %d sites: %d report, %d allowed, %d PENDING\n", r.sites, r.reported, r.allowed, r.pending);
    std::string kinds;
    for (const auto& k : r.byKind) { kinds += " " + k.first + "=" + std::to_string(k.second); }
    std::fprintf(out, "  by kind:%s\n", kinds.c_str());
    for (const auto& g : r.pendingByGroup) { std::fprintf(out, "  PENDING %-28s %d\n", g.first.c_str(), g.second); }
    for (const auto& m : r.unresolved) { std::fprintf(out, "FAIL %s\n", m.c_str()); }
    for (const auto& m : r.stale) { std::fprintf(out, "FAIL %s\n", m.c_str()); }
    for (const auto& m : r.problems) { std::fprintf(out, "FAIL %s\n", m.c_str()); }
}

// the tab-separated dump the seed tool reads: key, file, function, kind, reported(0/1), reporter, snippet, line (informational)
inline void dumpSites(const std::vector<Site>& sites, std::FILE* out) {
    for (const Site& s : sites) {
        std::fprintf(out, "%s\t%s\t%s\t%s\t%d\t%s\t%s\t%d\n", s.key.c_str(), s.file.c_str(), s.function.c_str(), s.kind.c_str(), s.reported ? 1 : 0,
                     s.reporter.c_str(), s.snippet.c_str(), s.line);
    }
}

}  // namespace errpath
