// test_engine_api_feed_check.cpp - THE HEADLESS FEED CHECK (lane feedplan; docs/feature-records/feedplan.md).
//
// For every row of a cases file: load ONE plugin module through the real engine (its own scratch plugin folder, a hermetic profile), open the row's I/Q WAV as radio 1's
// source (the same SELECT_SOURCE "file:" the window's Open button sends), tune it the way the row says (centre, mode, bandwidth, VFO offset), start the plugin on radio 1, let
// the file loop for at most the row's timeout and compare what the plugin printed (the engine's decoder-output backlog, FOXAPI_LIST_DECODER_LINES) with the row's expectation.
// The verdict is PASS, FAIL (some expectation never arrived in time) or SKIP (the module or the file is absent). A results table is printed and written as TSV.
//
//   test_engine_api_feed_check <cases.tsv> <plugin folder> <results.tsv> [id-or-prefix ...]       (no arguments after the third = every row)
//   test_engine_api_feed_check [--selftest]                                                      (the expectation language, no engine; what ctest runs)
//
// cases.tsv: one row per line (the wav path may start {FEED}, {DRIVE} or {SAMPLES}: see expandPath), TAB separated, '#' starts a comment. Columns:
//   id | module prefix ("cw-decoder-") | wav path | centre Hz | VFO offset Hz | mode (NFM WFM AM DSB USB CW LSB RAW) | bandwidth Hz | timeout s | expectation
// The expectation is a list of terms joined by " && ". A term is
//   text            a decoder line contains it (case-sensitive)
//   num:KEY=V~T     a decoder line holds KEY, then a number N with |N - V| <= T  (the TOLERANCE RULE: text is exact, numbers carry the tolerance the row states)
//   !text           NO decoder line contains it (absence; judged at the end of the wait, never early)
//   order:a|b|c     the tokens each occur in a later line than the one before (an ordered sequence, e.g. the keys of a DTMF string)
//   @lines>=N       at least N decoder lines in the backlog
//   (map tracks and pictures are NOT served by the engine API - FOXAPI_LIST_TRACKS answers UNSUPPORTED - so a track or picture is a by-hand check in the window, never a term here)
//   @markers>=N     at least N spectrum markers the plugin raised (FOXAPI_LIST_MARKERS)
// A row passes as soon as every positive term has been met (the file loops, so a missed burst comes round again); an absence term is checked at that moment.
//
// Nothing here opens a radio, a window, a network connection or the real profile (engine_api_test::hermetic points every per-user folder at a scratch folder).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;

namespace {
std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
fs::path g_plugins;
std::string pluginDirText() { return g_plugins.string(); }
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirText;
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

// ------------------------------------------------------------------------------------------------------------------------------------------------
// The expectation language (pure functions: --selftest runs them without an engine).
// ------------------------------------------------------------------------------------------------------------------------------------------------
struct Term {
    enum Kind { Text, Num, Absent, Lines, Markers, Order } kind = Text;
    std::string text;      // Text / Absent: the needle; Num: the key
    double value = 0.0;    // Num: the value; counts: N
    double tol = 0.0;
    std::string raw;
    bool met = false;
};

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::string cur;
    for (char x : s) {
        if (x == c) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += x;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\r' || s[a] == '\n')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r' || s[b - 1] == '\n')) { --b; }
    return s.substr(a, b - a);
}

std::vector<Term> parseExpect(const std::string& text) {
    std::vector<Term> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t e = text.find(" && ", pos);
        const std::string raw = trim(text.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        pos = e == std::string::npos ? text.size() + 1 : e + 4;
        if (raw.empty()) { continue; }
        Term t;
        t.raw = raw;
        if (raw.rfind("num:", 0) == 0) {
            const std::string body = raw.substr(4);
            const std::size_t eq = body.find('=');
            const std::size_t tl = body.find('~', eq == std::string::npos ? 0 : eq);
            if (eq != std::string::npos && tl != std::string::npos) {
                t.kind = Term::Num;
                t.text = body.substr(0, eq);
                t.value = std::atof(body.substr(eq + 1, tl - eq - 1).c_str());
                t.tol = std::atof(body.substr(tl + 1).c_str());
            } else {
                t.kind = Term::Text;   // malformed numeric term: keep it as literal text so it fails loudly
                t.text = raw;
            }
        } else if (raw.rfind("order:", 0) == 0) {
            t.kind = Term::Order;
            t.text = raw.substr(6);
        } else if (raw[0] == '!') {
            t.kind = Term::Absent;
            t.text = raw.substr(1);
        } else if (raw.rfind("@lines>=", 0) == 0) {
            t.kind = Term::Lines;
            t.value = std::atof(raw.c_str() + 8);
        } else if (raw.rfind("@markers>=", 0) == 0) {
            t.kind = Term::Markers;
            t.value = std::atof(raw.c_str() + 10);
        } else {
            t.text = raw;
        }
        out.push_back(t);
    }
    return out;
}

// The first number that follows `key` in `line` (an optional '=' or ':' and spaces between).
bool numberAfter(const std::string& line, const std::string& key, double& out) {
    std::size_t from = 0;
    while (true) {
        const std::size_t at = line.find(key, from);
        if (at == std::string::npos) { return false; }
        std::size_t p = at + key.size();
        while (p < line.size() && (line[p] == '=' || line[p] == ':' || line[p] == ' ')) { ++p; }
        const char* b = line.c_str() + p;
        char* end = nullptr;
        const double v = std::strtod(b, &end);
        if (end != b) {
            out = v;
            return true;
        }
        from = at + 1;
    }
}

bool lineMeets(const Term& t, const std::string& line) {
    if (t.kind == Term::Text || t.kind == Term::Absent) { return line.find(t.text) != std::string::npos; }
    if (t.kind == Term::Num) {
        double n = 0.0;
        return numberAfter(line, t.text, n) && std::fabs(n - t.value) <= t.tol;
    }
    return false;
}

struct Seen {
    std::vector<std::string> lines;
    int markers = 0;
};

// Updates `met` for every positive term; returns true when all positive terms are met. Absence terms are NOT part of the early exit (see finalAbsence).
bool evaluate(std::vector<Term>& terms, const Seen& s) {
    bool all = true;
    for (Term& t : terms) {
        if (t.kind == Term::Absent) { continue; }
        if (!t.met) {
            if (t.kind == Term::Lines) {
                t.met = static_cast<double>(s.lines.size()) >= t.value;
            } else if (t.kind == Term::Order) {
                // tokens separated by '|' must each be found in a LATER line than the one before (an ordered sequence, e.g. the keys of a DTMF string)
                std::size_t from = 0;
                bool all = true;
                for (const std::string& tok : split(t.text, '|')) {
                    bool found = false;
                    for (std::size_t li = from; li < s.lines.size(); ++li) {
                        if (s.lines[li].find(tok) != std::string::npos) {
                            from = li + 1;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        all = false;
                        break;
                    }
                }
                t.met = all;
            } else if (t.kind == Term::Markers) {
                t.met = static_cast<double>(s.markers) >= t.value;
            } else {
                for (const std::string& l : s.lines) {
                    if (lineMeets(t, l)) {
                        t.met = true;
                        break;
                    }
                }
            }
        }
        all = all && t.met;
    }
    return all;
}

bool absenceHolds(std::vector<Term>& terms, const Seen& s) {
    bool ok = true;
    for (Term& t : terms) {
        if (t.kind != Term::Absent) { continue; }
        t.met = true;
        for (const std::string& l : s.lines) {
            if (lineMeets(t, l)) {
                t.met = false;
                ok = false;
            }
        }
    }
    return ok;
}

int selftest() {
    int failed = 0;
    const auto expect = [&](bool c, const char* what) {
        std::printf("  %s %s\n", c ? "ok  " : "FAIL", what);
        if (!c) { ++failed; }
    };
    std::vector<Term> t = parseExpect("FOXSDR && num:rms=0.34~0.02 && !error && @lines>=2 && @markers>=1");
    expect(t.size() == 5, "five terms parsed");
    Seen s;
    s.lines = {"example: 8000 samples, rms 0.34", "FOXSDR fixture"};
    expect(!evaluate(t, s), "markers term unmet keeps the row open");
    s.markers = 1;
    expect(evaluate(t, s), "all positive terms met");
    expect(absenceHolds(t, s), "absence term holds");
    s.lines.push_back("error: boom");
    expect(!absenceHolds(t, s), "absence term fails when the text appears");
    std::vector<Term> n = parseExpect("num:temp=21.7~0.1");
    Seen a;
    a.lines = {"id=F824 temp=21.9C"};
    expect(!evaluate(n, a), "number outside the tolerance is a miss (21.9 vs 21.7 +-0.1)");
    a.lines = {"id=F824 temp=21.75C"};
    expect(evaluate(n, a), "number inside the tolerance is a hit");
    std::vector<Term> ord = parseExpect("order:s  1|s  2|s  3");
    Seen os;
    os.lines = {"+0.1 s  1", "+0.2 s  3", "+0.3 s  2"};
    expect(!evaluate(ord, os), "an ordered sequence out of order is a miss");
    os.lines.push_back("+0.4 s  3");
    expect(evaluate(ord, os), "an ordered sequence found in order is a hit");
    std::vector<Term> b = parseExpect("num:broken");
    Seen e;
    e.lines = {"anything"};
    expect(!evaluate(b, e), "a malformed numeric term never passes");
    expect(parseExpect("").empty(), "an empty expectation has no terms");
    double v = 0.0;
    expect(numberAfter("DEMOD QPSK, 48.00 kBd, carrier +190.000 kHz", "carrier", v) && std::fabs(v - 190.0) < 1e-9, "number after a key with a sign");
    expect(!numberAfter("no digits here", "digits", v), "key without a number");
    std::printf("%s (%d failed)\n", failed == 0 ? "selftest ok" : "selftest FAILED", failed);
    return failed == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------
// The rig: one engine, one radio, one plugin.
// ------------------------------------------------------------------------------------------------------------------------------------------------
struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;
    Rig() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        pump(3);
    }
    bool ok() const { return s && s->ok(); }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class F>
    bool until(F done, int timeoutMs) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    testing::Outcome send(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxReceiverState radio(std::uint32_t n) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        (void)api()->read_radio_state(s->raw(), n, &st);
        return st;
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
    int count(std::uint32_t list) { return api()->read_list(s->raw(), list, 0, nullptr); }
    void snapshot(Seen& out) {
        out.lines.clear();
        const int32_t n = count(FOXAPI_LIST_DECODER_LINES);
        for (int32_t i = 0; i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (api()->read_list(s->raw(), FOXAPI_LIST_DECODER_LINES, static_cast<std::uint32_t>(i), &it) <= i) { break; }
            out.lines.push_back(it.detail);
        }
        out.markers = std::max(0, count(FOXAPI_LIST_MARKERS));
    }
};

struct Row {
    std::string id, prefix, wav;
    double centre = 0, vfo = 0, bw = 0;
    std::string mode;
    int timeoutS = 30;
    std::string expect;
};

std::uint32_t modeOf(const std::string& m) {
    if (m == "NFM") { return FOXAPI_DEMOD_NFM; }
    if (m == "WFM") { return FOXAPI_DEMOD_WFM; }
    if (m == "AM") { return FOXAPI_DEMOD_AM; }
    if (m == "DSB") { return FOXAPI_DEMOD_DSB; }
    if (m == "USB") { return FOXAPI_DEMOD_USB; }
    if (m == "CW") { return FOXAPI_DEMOD_CW; }
    if (m == "LSB") { return FOXAPI_DEMOD_LSB; }
    return FOXAPI_DEMOD_RAW;
}

std::string moduleFor(const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    std::string best;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".dll" && n > best) { best = n; }
    }
    return best;
}

std::string clip(const std::string& s, std::size_t n) { return s.size() <= n ? s : s.substr(0, n - 3) + "..."; }

struct Result {
    std::string id, verdict, module, fragment, note;
    double seconds = 0;
    int lines = 0, markers = 0;
    std::string unmet;
};

Result runRow(const Row& row, const fs::path& pluginSrc) {
    Result r;
    r.id = row.id;
    const std::string mod = moduleFor(pluginSrc, row.prefix);
    r.module = mod;
    std::error_code ec;
    if (mod.empty()) {
        r.verdict = "SKIP";
        r.note = "not built in this tree (no module '" + row.prefix + "*.dll' in " + pluginSrc.string() + ")";
        return r;
    }
    if (!fs::is_regular_file(row.wav, ec)) {
        r.verdict = "SKIP";
        r.note = "feed file absent: " + row.wav;
        return r;
    }
    // its own plugin folder: only this module, so no other plugin's lines or faults can reach the row
    g_plugins = fs::temp_directory_path() / ("foxsdr_feed_plugins_" + std::to_string(
#if defined(_WIN32)
                                                                      _getpid()
#else
                                                                      getpid()
#endif
                                                                          ) + "_" + row.id);
    fs::remove_all(g_plugins, ec);
    fs::create_directories(g_plugins, ec);
    fs::copy_file(pluginSrc / mod, g_plugins / mod, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        r.verdict = "FAIL";
        r.note = "could not copy the module: " + ec.message();
        return r;
    }
    const auto t0 = std::chrono::steady_clock::now();
    {
        Rig rig;
        if (!rig.ok()) {
            r.verdict = "FAIL";
            r.note = "engine session did not open";
        } else {
            const auto tune = [&] {
                rig.send(rig.aimed(FOXAPI_OP_SET_CENTRE, 1, row.centre));
                rig.send(rig.aimed(FOXAPI_OP_SET_MODE, 1, 0.0, modeOf(row.mode)));
                if (row.bw > 0) { rig.send(rig.aimed(FOXAPI_OP_SET_BANDWIDTH, 1, row.bw)); }
                rig.send(rig.aimed(FOXAPI_OP_SET_VFO_OFFSET, 1, row.vfo));
                rig.send(rig.aimed(FOXAPI_OP_SET_VOLUME, 1, 0.0));   // nothing plays
            };
            const int sel = rig.send(rig.aimed(FOXAPI_OP_SELECT_SOURCE, 1, 0.0, 0, ("file:" + row.wav).c_str())).status();
            if (sel != FOXAPI_OK) {
                r.verdict = "FAIL";
                r.note = "the engine refused the I/Q file (status " + std::to_string(sel) + ")";
            } else if (!rig.until([&] { return rig.fact("radio.1.source") == "file" && rig.radio(1).sampleRateHz > 0.0; }, 20000)) {
                r.verdict = "FAIL";
                r.note = "the I/Q file did not open within 20 s";
            } else {
                tune();
                rig.send(rig.aimed(FOXAPI_OP_RUN, 1, 0.0, 1));
                FoxCommand p = testing::cmd(FOXAPI_OP_PLUGIN_RADIO, 0.0, 1, mod.c_str());
                p.ival[1] = 1;
                const int pst = rig.send(p).status();
                FoxCommand st = testing::cmd(FOXAPI_OP_DECODER_START, 0.0, 0, mod.c_str());
                (void)rig.send(st);
                rig.pump(5);
                tune();   // START may apply the plugin's first preset: put the tuning back the way the row says
                std::vector<Term> terms = parseExpect(row.expect);
                Seen seen;
                bool met = false;
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(row.timeoutS);
                while (std::chrono::steady_clock::now() < end) {
                    rig.pump(10);
                    rig.snapshot(seen);
                    if (evaluate(terms, seen)) {
                        met = true;
                        break;
                    }
                }
                if (met) { rig.pump(40); rig.snapshot(seen); }
                const bool absent = absenceHolds(terms, seen);
                r.lines = static_cast<int>(seen.lines.size());
                r.markers = seen.markers;
                for (const Term& t : terms) {
                    if (!t.met) { r.unmet += (r.unmet.empty() ? "" : " && ") + t.raw; }
                }
                // the fragment: the lines that satisfied a term first, then the first lines
                std::vector<std::string> pick;
                for (const Term& t : terms) {
                    if (t.kind == Term::Text || t.kind == Term::Num) {
                        for (const std::string& l : seen.lines) {
                            if (lineMeets(t, l) && std::find(pick.begin(), pick.end(), l) == pick.end()) {
                                pick.push_back(l);
                                break;
                            }
                        }
                    }
                }
                for (const std::string& l : seen.lines) {
                    if (pick.size() >= 3) { break; }
                    if (std::find(pick.begin(), pick.end(), l) == pick.end()) { pick.push_back(l); }
                }
                for (std::size_t i = 0; i < pick.size() && i < 4; ++i) { r.fragment += (i ? " | " : "") + clip(pick[i], 150); }
                r.verdict = (met && absent) ? "PASS" : "FAIL";
                if (!met) { r.note = "unmet: " + r.unmet; }
                if (met && !absent) { r.note = "a forbidden line appeared"; }
                if (pst != FOXAPI_OK && pst != FOXAPI_NO_CHANGE) { r.note += (r.note.empty() ? "" : "; ") + std::string("PLUGIN_RADIO answered ") + std::to_string(pst); }
            }
        }
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fs::remove_all(g_plugins, ec);
    return r;
}

// {FEED}, {DRIVE} and {SAMPLES} in a wav path are the environment variables FEED_DIR (the generated fixtures), DRIVE_DIR (the slint drive set made by make_*.py) and SAMPLES_DIR
// (the local-only real recordings; never committed).
std::string expandPath(std::string p) {
    for (const char* key : {"FEED", "DRIVE", "SAMPLES"}) {
        const std::string token = std::string("{") + key + "}";
        const char* v = std::getenv((std::string(key) + "_DIR").c_str());
        for (std::size_t at; (at = p.find(token)) != std::string::npos;) { p.replace(at, token.size(), v != nullptr ? v : ""); }
    }
    return p;
}

std::vector<Row> readCases(const fs::path& file) {
    std::vector<Row> rows;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty() || line[0] == '#') { continue; }
        const std::vector<std::string> c = split(line, '\t');
        if (c.size() < 9) {
            std::printf("  (skipping a short row: %s)\n", clip(line, 80).c_str());
            continue;
        }
        Row r;
        r.id = c[0];
        r.prefix = c[1];
        r.wav = expandPath(c[2]);
        r.centre = std::atof(c[3].c_str());
        r.vfo = std::atof(c[4].c_str());
        r.mode = c[5];
        r.bw = std::atof(c[6].c_str());
        r.timeoutS = std::max(1, std::atoi(c[7].c_str()));
        r.expect = c[8];
        rows.push_back(r);
    }
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2 || std::string(argv[1]) == "--selftest") { return selftest(); }   // no arguments (ctest): the expectation language only
    std::printf("test_engine_api_feed_check\n");
    if (argc < 4) {
        std::printf("usage: test_engine_api_feed_check <cases.tsv> <plugin folder> <results.tsv> [id-or-prefix ...] | --selftest\n");
        return 2;
    }
    const fs::path casesFile = argv[1];
    const fs::path pluginSrc = argv[2];
    const fs::path resultsFile = argv[3];
    std::vector<std::string> only;
    for (int i = 4; i < argc; ++i) { only.push_back(argv[i]); }
    engine_api_test::hermetic("feed_check");   // after the arguments are read: every per-user folder is now a scratch folder
    std::vector<Row> rows = readCases(casesFile);
    if (!only.empty()) {
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Row& r) {
                                      for (const std::string& o : only) {
                                          if (r.id.rfind(o, 0) == 0) { return false; }
                                      }
                                      return true;
                                  }),
                   rows.end());
    }
    std::printf("%zu rows\n", rows.size());
    Access::installHooks();
    std::vector<Result> results;
    int pass = 0, fail = 0, skip = 0;
    for (const Row& row : rows) {
        Result r = runRow(row, pluginSrc);
        std::printf("%-5s %-34s %6.1fs  lines=%d markers=%d  %s%s\n", r.verdict.c_str(), r.id.c_str(), r.seconds, r.lines, r.markers, r.note.empty() ? "" : "-- ", r.note.c_str());
        if (!r.fragment.empty()) { std::printf("        %s\n", r.fragment.c_str()); }
        (r.verdict == "PASS" ? pass : r.verdict == "SKIP" ? skip : fail)++;
        results.push_back(r);
    }
    std::ofstream out(resultsFile);
    out << "id\tverdict\tmodule\tseconds\tlines\tmarkers\tnote\tfragment\n";
    for (const Result& r : results) {
        out << r.id << '\t' << r.verdict << '\t' << r.module << '\t' << static_cast<int>(r.seconds + 0.5) << '\t' << r.lines << '\t' << r.markers << '\t' << r.note << '\t' << r.fragment << '\n';
    }
    std::printf("feed check: %d PASS, %d FAIL, %d SKIP\n", pass, fail, skip);
    return fail == 0 ? 0 : 1;
}
