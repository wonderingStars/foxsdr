// test_radio_families_sinks.cpp - source scan for the radio-family sources of the feat/radio-families lane
// (ascp_*, hpsdr1_*, aaronia_*): none of them may print to stdout/stderr, write to the diagnostic log, or write
// a setting. These sources handle identifiers of devices (serial numbers, MAC addresses, server names); the
// host captures process stderr into its log and uploaded reports and writes every settings value to the config
// file (STANDARD-RULES.md, "Host sinks"), so an identifier must never travel to either. Error sentences that
// reach the caller through lastError() name the device only by what the user typed (the address) or by a
// printable, length-bounded copy of the radio's own name (see tame() in ascp_source.cpp), and the caller decides
// what to show.
//
// The scan drops // comments and the contents of string literals before looking, so documentation text cannot
// trip it; a planted violation proves the scan can fail.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_check.hpp"

#ifndef CASCADE_SOURCE_DIR
#error "CASCADE_SOURCE_DIR must be defined by the test registration"
#endif

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream b;
    b << in.rdbuf();
    return b.str();
}

// Removes // comments, /* */ comments and the insides of "..." and '...' literals (escapes honoured).
std::string codeOnly(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') { ++i; }
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { ++i; }
            i += 2;
        } else if (c == '"' || c == '\'') {
            const char q = c;
            out += q;
            ++i;
            while (i < s.size() && s[i] != q) { i += (s[i] == '\\') ? 2 : 1; }
            out += q;
            ++i;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

const std::vector<std::string> kForbidden = {
    "stderr", "stdout", "std::cerr", "std::cout", "std::clog", "printf(", "fprintf(", "puts(", "fputs(", "fwrite(", "perror(",
    "OutputDebugString", "diagLog", "diagWarn", "diagErr", "settings_set", "setSetting", "writeSetting", "std::system(", "popen(",
};

std::vector<std::string> violationsIn(const std::string& text) {
    std::vector<std::string> v;
    const std::string code = codeOnly(text);
    for (const std::string& tok : kForbidden) {
        // snprintf( into a local buffer is formatting, not output: allow it
        std::size_t pos = 0;
        while ((pos = code.find(tok, pos)) != std::string::npos) {
            const bool snp = tok == "printf(" && pos >= 1 && code[pos - 1] == 'n' && pos >= 2 && code[pos - 2] == 's';
            const bool vsn = tok == "printf(" && pos >= 3 && code.compare(pos - 3, 3, "vsn") == 0;
            const bool word = pos == 0 || !(std::isalnum(static_cast<unsigned char>(code[pos - 1])) || code[pos - 1] == '_');
            if (!snp && !vsn && (word || tok.back() != '(')) { v.push_back(tok); }
            pos += tok.size();
        }
    }
    return v;
}

}  // namespace

int main() {
    const std::string root = std::string(CASCADE_SOURCE_DIR) + "/src/source/";
    const std::vector<std::string> files = {"ascp_protocol.hpp", "ascp_source.hpp", "ascp_source.cpp", "hpsdr1_frames.hpp",
                                            "hpsdr1_source.hpp", "hpsdr1_source.cpp", "aaronia_protocol.hpp", "aaronia_source.hpp",
                                            "aaronia_source.cpp"};
    for (const std::string& f : files) {
        const std::string text = slurp(root + f);
        CHECK(text.size() > 500);  // the file was found and read
        const std::vector<std::string> v = violationsIn(text);
        for (const std::string& tok : v) { std::printf("VIOLATION in %s: %s\n", f.c_str(), tok.c_str()); }
        CHECK(v.empty());
    }
    // the scan itself: planted violations are found, comments and string literals are not
    CHECK(!violationsIn("void f() { fprintf(stderr, \"x\"); }").empty());
    CHECK(!violationsIn("void f() { std::cerr << serial; }").empty());
    CHECK(!violationsIn("void f() { printf(\"%s\", mac); }").empty());
    CHECK(!violationsIn("void f() { core::diagLogf(\"serial %s\", s); }").empty());
    CHECK(!violationsIn("void f() { host.settings_set(\"serial\", s); }").empty());
    CHECK(violationsIn("void f() { /* printf( stderr */ // stdout\n int x = 1; const char* s = \"stderr printf( diagLog\"; }").empty());
    CHECK(violationsIn("void f() { char b[8]; snprintf(b, 8, \"%d\", 1); }").empty());
    CHECK(violationsIn("// printf(\nint g;").empty());
    return testSummary("test_radio_families_sinks");
}
