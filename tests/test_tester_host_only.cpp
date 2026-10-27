// A HEADLESS ENGINE CAN NEVER SEND A TESTER REPORT (stub campaign B15; the owner's decision O8, the stage-7 survey section 8, docs/engine-stage4.md 6.22 B15).
//
// THE PROPERTY. The beta tester's usage record is the WINDOW's: the recorder, the retry queue, the sender, the confirm-by-name lookup and the portal exchange are host
// glue (src/gui/app_window_tester.cpp), fed from facts the table publishes and from the window's own events. The engine - which a service runs with no window, on a
// machine whose owner may have pasted a code into the settings file the desktop shares - must have no way to send one. This holds that from five sides, because "the
// engine does not call it" is the kind of sentence that stays true until the day it does not:
//
//   1. THE SOURCES. No file of the engine (src/engine, src/engine_main.cpp) or of the servers (src/net) names the recorder, the sender, the lookup, the exchange or the
//      poll of the link-request file; `setUsageCallback` - the servers' own seam for the recorder's two marks - is called from exactly one place, the door
//      (EngineShell::setServerUsageHooks), and that door is called from exactly one place, the window. (Comments and string literals are blanked first.)
//   2. THE PROCESS BOOT. The link-request file is written in one place (STEP 5), after the Host-only kind gate; the engine's own main names no part of the link flow.
//   3. THE BUILT IMAGES. cascade_engine is a static library and a binary takes only the objects it references, so the engine binary links the token's shape and the
//      retry queue's rules (the settings file is validated with them) and not the session half of the record or the portal's network half (tester_usage_session.cpp,
//      tester_link_net.cpp: split out for exactly this). The endpoint of the usage report and the paths of the portal are in cascade.exe - the control, so the search
//      is shown to find them - and NOT in foxsdr-engine.exe.
//   4. THE RUNNING ENGINE. foxsdr-engine over a settings file that carries a code, a report left by a session and a queue, with the usage endpoint and the portal's
//      address both pointed at a stand-in on loopback: it ends cleanly, the stand-in is never asked anything, and the file's tester fields come back as they went in.
//   5. THE RUNNING WINDOW, as the control: the same file, run through cascade, sends the left report to the stand-in and empties the queue - so a stand-in that
//      heard nothing in 4 is a stand-in that would have heard it - and, with the web server and the CAT listener on, a browser request and a CAT client are marks in
//      the report (the window's hooks reached the engine's servers through the door).
//
// HERMETIC: every per-user folder is a scratch tree, the settings files are scratch paths named on the command line or in CASCADE_CONFIG_TEST, every other
// FOXSDR_*_URL points at a closed local port, and the real profile is never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include <nlohmann/json.hpp>

#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// --- the sources -----------------------------------------------------------------------------------------------------------------------------------------

// The code with comments and string/character literals blanked (line structure kept): what is left is what the compiler sees as tokens.
std::string codeOnly(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        const char d = i + 1 < n ? s[i + 1] : '\0';
        if (c == '/' && d == '/') {
            while (i < n && s[i] != '\n') { ++i; }
        } else if (c == '/' && d == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') { out.push_back('\n'); }
                ++i;
            }
            i += 2;
        } else if (c == '"' || c == '\'') {
            out.push_back(c);
            ++i;
            while (i < n && s[i] != c) {
                if (s[i] == '\\') { ++i; }
                if (i < n && s[i] == '\n') { out.push_back('\n'); }
                ++i;
            }
            out.push_back(c);
            ++i;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

bool isWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// How many times `ident` stands as a whole word in `code`.
int countWord(const std::string& code, const std::string& ident) {
    int n = 0;
    for (std::size_t at = code.find(ident); at != std::string::npos; at = code.find(ident, at + 1)) {
        const bool leftOk = at == 0 || !isWord(code[at - 1]);
        const std::size_t end = at + ident.size();
        const bool rightOk = end >= code.size() || !isWord(code[end]);
        if (leftOk && rightOk) { ++n; }
    }
    return n;
}

fs::path findRepoRoot(const std::string& given) {
    if (!given.empty()) { return given; }
    fs::path dir = fs::current_path();
    std::error_code ec;
    for (int level = 0; !ec && level < 10; ++level) {
        if (fs::is_regular_file(dir / "PRIVACY.md", ec) && fs::is_directory(dir / "src", ec)) { return dir; }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

struct Source {
    std::string rel;   // "src/engine/engine.cpp"
    std::string code;  // comments and literals blanked
};

std::vector<Source> sourcesUnder(const fs::path& root, const char* dir) {
    std::vector<Source> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) { continue; }
        const std::string ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        Source s;
        s.rel = fs::relative(it->path(), root, ec).generic_string();
        s.code = codeOnly(emt::readFile(it->path()));
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const Source& a, const Source& b) { return a.rel < b.rel; });
    return out;
}

// The text of the function whose definition starts at `head`, brace to brace (code already blanked of braces in literals).
std::string bodyAt(const std::string& code, const std::string& head) {
    const std::size_t at = code.find(head);
    if (at == std::string::npos) { return {}; }
    const std::size_t open = code.find('{', at);
    if (open == std::string::npos) { return {}; }
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i) {
        if (code[i] == '{') { ++depth; }
        if (code[i] == '}' && --depth == 0) { return code.substr(open, i - open + 1); }
    }
    return {};
}

void testSources(const fs::path& root) {
    std::printf("  the sources\n");
    const std::vector<Source> engine = sourcesUnder(root, "src/engine");
    const std::vector<Source> net = sourcesUnder(root, "src/net");
    const std::vector<Source> gui = sourcesUnder(root, "src/gui");
    const std::vector<Source> core = sourcesUnder(root, "src/core");
    CHECK(!engine.empty() && !net.empty() && !gui.empty() && !core.empty());
    std::vector<Source> engineSide = engine;
    engineSide.insert(engineSide.end(), net.begin(), net.end());
    {
        Source m;
        m.rel = "src/engine_main.cpp";
        m.code = codeOnly(emt::readFile(root / "src" / "engine_main.cpp"));
        CHECK(!m.code.empty());
        engineSide.push_back(std::move(m));
    }

    // 1a. The record and the means of sending it are named by no engine-side file. (The door, engine_shell.*, is engine-side too: it names none of them either - it
    //     hands over closures it does not look into.)
    const char* const kHostOnly[] = {"TesterUsageRecorder", "TesterUsageSender", "TesterUsageReport", "TesterUsageOutcome", "postTesterUsage",
                                     "testerUsageEndpoint", "BetaLinkNameSender", "BetaMigrationSender", "resolveAppTokenName", "exchangePortalToken",
                                     "betaApiBaseUrl", "LinkRequestPoll", "claimLinkRequestFile", "noteFeature", "noteRadio", "accruePlugin"};
    for (const Source& s : engineSide) {
        for (const char* id : kHostOnly) {
            const int n = countWord(s.code, id);
            if (n != 0) { std::printf("    FAIL %s names %s (%d)\n", s.rel.c_str(), id, n); }
            CHECK(n == 0);
        }
    }

    // 1b. setUsageCallback: the servers define it, the door calls it - twice, once a server, inside setServerUsageHooks - and nobody else does.
    const std::set<std::string> definers = {"src/net/web_server.cpp", "src/net/web_server.hpp", "src/net/cat_server.cpp", "src/net/cat_server.hpp"};
    int doorCalls = 0;
    for (const std::vector<Source>* group : {&engine, &net, &gui, &core}) {
        for (const Source& s : *group) {
            const int n = countWord(s.code, "setUsageCallback");
            if (n == 0) { continue; }
            if (definers.count(s.rel) != 0) { continue; }
            if (s.rel == "src/engine/engine_shell.cpp") {
                doorCalls += n;
                continue;
            }
            std::printf("    FAIL %s calls setUsageCallback (%d)\n", s.rel.c_str(), n);
            CHECK(false);
        }
    }
    CHECK(doorCalls == 2);
    for (const Source& s : engine) {
        if (s.rel != "src/engine/engine_shell.cpp") { continue; }
        const std::string body = bodyAt(s.code, "bool EngineShell::setServerUsageHooks(");
        CHECK(!body.empty());
        CHECK(countWord(body, "setUsageCallback") == 2);
    }

    // 1c. The door has exactly one caller outside its own two files: the window's tester file, from one function.
    int doorCallers = 0;
    for (const std::vector<Source>* group : {&engine, &net, &gui, &core}) {
        for (const Source& s : *group) {
            const int n = countWord(s.code, "setServerUsageHooks");
            if (n == 0 || s.rel == "src/engine/engine_shell.hpp" || s.rel == "src/engine/engine_shell.cpp") { continue; }
            if (s.rel == "src/gui/app_window_tester.cpp") {
                doorCallers += n;
                continue;
            }
            std::printf("    FAIL %s calls the tester hooks' door (%d)\n", s.rel.c_str(), n);
            CHECK(false);
        }
    }
    CHECK(doorCallers == 1);
    for (const Source& s : gui) {
        if (s.rel != "src/gui/app_window_tester.cpp") { continue; }
        const std::string body = bodyAt(s.code, "void AppWindow::installTesterUsageHooks(");
        CHECK(!body.empty());
        CHECK(countWord(body, "setServerUsageHooks") == 1);
    }

    // 1d. The core files that hold the means of sending are in no engine-side include: a header that declares them (core/tester_usage.hpp, core/tester_link.hpp)
    //     is included by the window, by config.cpp (the token's shape, the queue) and by the process boot (the link file) - and by nothing else on the engine side.
    const std::set<std::string> mayIncludeLinkHeader = {"src/engine/process_boot.cpp"};
    for (const Source& s : engineSide) {
        const std::string text = emt::readFile(root / s.rel);
        // (An #include, not a mention: the door's comment names the header it is about.)
        const bool usage = text.find("#include \"core/tester_usage.hpp\"") != std::string::npos;
        const bool link = text.find("#include \"core/tester_link.hpp\"") != std::string::npos;
        if (usage) { std::printf("    FAIL %s includes core/tester_usage.hpp\n", s.rel.c_str()); }
        if (link && mayIncludeLinkHeader.count(s.rel) == 0) { std::printf("    FAIL %s includes core/tester_link.hpp\n", s.rel.c_str()); }
        CHECK(!usage);
        CHECK(!link || mayIncludeLinkHeader.count(s.rel) != 0);
    }
}

void testProcessBoot(const fs::path& root) {
    std::printf("  the process boot\n");
    const std::string boot = codeOnly(emt::readFile(root / "src" / "engine" / "process_boot.cpp"));
    CHECK(!boot.empty());
    // The link-request file is written exactly once, inside activateLink, AFTER the Host-only kind gate.
    CHECK(countWord(boot, "writeLinkRequestFile") == 1);
    const std::string body = bodyAt(boot, "Step activateLink(");
    CHECK(!body.empty());
    const std::size_t gate = body.find("kind != ProcessKind::Host");
    const std::size_t write = body.find("writeLinkRequestFile");
    CHECK(gate != std::string::npos && write != std::string::npos && gate < write);
    // ...and the gate returns before anything is claimed or written (the first statement of its block is the log, the last is the return).
    const std::size_t ret = body.find("return std::nullopt;", gate);
    CHECK(ret != std::string::npos && ret < write);
    // The engine's own main names no part of the link flow.
    const std::string mainCode = codeOnly(emt::readFile(root / "src" / "engine_main.cpp"));
    for (const char* id : {"writeLinkRequestFile", "claimLinkRequestFile", "parseBetaLinkUrl", "claimPrimaryInstance", "linkRequestPath"}) {
        CHECK(countWord(mainCode, id) == 0);
    }
}

// --- the built images --------------------------------------------------------------------------------------------------------------------------------------

bool imageHas(const std::string& image, const std::string& needle) { return image.find(needle) != std::string::npos; }

void testImages() {
    std::printf("  the built images\n");
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    const fs::path windowExe = emt::appExe("cascade");
    const std::string engineImage = emt::readFile(engineExe);
    const std::string windowImage = emt::readFile(windowExe);
    CHECK(!engineImage.empty());
    CHECK(!windowImage.empty());
    // What only the sending half carries: the usage report's address, and the two paths of the portal.
    const char* const kSendingHalf[] = {"https://foxsdr.com/api/tester-usage", "/api/beta/app-token/me", "/api/beta/app-token"};
    for (const char* needle : kSendingHalf) {
        // THE CONTROL: the window's image has it, so the search finds what it looks for.
        const bool inWindow = imageHas(windowImage, needle);
        const bool inEngine = imageHas(engineImage, needle);
        std::printf("    %-40s cascade: %s   foxsdr-engine: %s\n", needle, inWindow ? "yes" : "NO", inEngine ? "YES" : "no");
        CHECK(inWindow);
        CHECK(!inEngine);
    }
}

// --- the stand-in site -------------------------------------------------------------------------------------------------------------------------------------

// A loopback site that answers every request 200 and keeps what it was asked: method, path and the bearer.
class StandIn {
public:
    struct Request {
        std::string method, path, auth, body;
    };
    // `status` is what every request is answered with (the site's refusal of a code is 401).
    bool start(int status = 200) {
        status_ = status;
        const auto record = [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                requests_.push_back({req.method, req.path, req.get_header_value("Authorization"), req.body});
            }
            res.status = status_;
            res.set_content("{}", "application/json");
        };
        server_.Get(".*", record);
        server_.Post(".*", record);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) { return false; }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return true;
    }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<Request> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        requests_.clear();
    }
    ~StandIn() {
        server_.stop();
        if (thread_.joinable()) { thread_.join(); }
    }

private:
    httplib::Server server_;
    int port_ = 0;
    int status_ = 200;
    std::thread thread_;
    std::mutex mu_;
    std::vector<Request> requests_;
};

int freePort() {
    httplib::Server s;
    const int p = s.bind_to_any_port("127.0.0.1");
    s.stop();
    return p;
}

const std::string kToken = "0123456789abcdef0123456789abcdef";

std::string reportJson(const std::string& token, const char* feature) {
    json j;
    j["token"] = token;
    j["version"] = "0.99.90";
    j["platform"] = "windows";
    j["arch"] = "x64";
    j["session"] = {{"start", "2026-10-26T10:00:00Z"}, {"minutes", 5}};
    j["features"] = json::array({feature});
    j["plugins"] = json::array();
    j["radios"] = json::array();
    return j.dump();
}

// The settings file every run below is given: a code, the report the last session left, one report already queued - and, for the window run, the servers on.
void writeSettings(const fs::path& path, int webPort, int catPort) {
    json c;
    c["testerToken"] = kToken;
    c["testerUsageCurrent"] = reportJson(kToken, "left-by-the-last-session");
    c["testerUsagePending"] = json::array({reportJson(kToken, "already-queued")});
    c["telemetryEnabled"] = false;
    c["updateCheckEnabled"] = false;
    c["sourceKind"] = "siggen";
    c["volume"] = 0.0;
    c["railBank"] = 0;
    c["mainView"] = "receiver";  // the default is the patch page, which draws no spectrum: the receiver view is a mark of its own
    if (webPort > 0) {
        c["webEnabled"] = true;
        c["webBindAddress"] = "127.0.0.1";
        c["webPort"] = webPort;
    }
    if (catPort > 0) {
        c["catEnabled"] = true;
        c["catBindAll"] = false;
        c["catPort"] = catPort;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << c.dump(2) << "\n";
}

json readSettings(const fs::path& path) { return json::parse(emt::readFile(path), nullptr, false); }

fs::path g_home;

void closedPorts() {
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_CATALOGUE_URL", "FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL"}) {
        emt::setEnv(url, "http://127.0.0.1:9/");
    }
}

void testEngineSendsNothing() {
    std::printf("  the running engine, with a code in its settings\n");
    StandIn site;
    CHECK(site.start());
    closedPorts();
    // BOTH of the ways a tester report goes out are pointed at the stand-in: the usage endpoint and the portal (the migration exchange of a portal code).
    emt::setEnv("FOXSDR_TESTER_USAGE_URL", site.base() + "/api/tester-usage");
    emt::setEnv("FOXSDR_BETA_API_URL", site.base());
    const fs::path cfg = g_home / "engine" / "config.json";
    writeSettings(cfg, 0, 0);
    const json before = readSettings(cfg);
    const emt::RunResult r = emt::run(emt::appExe("foxsdr-engine"), {"--config", cfg.string(), "--seconds", "4"}, g_home / "engine", g_home / "engine-out", "run", 120000);
    CHECK(r.started);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 0);
    CHECK(emt::contains(r.err, "settings file in use (restored)"));
    // NEVER ASKED: not for the left report, not for the queue, not for a portal exchange.
    const std::vector<StandIn::Request> heard = site.requests();
    for (const StandIn::Request& q : heard) { std::printf("    the stand-in was asked: %s %s\n", q.method.c_str(), q.path.c_str()); }
    CHECK(heard.empty());
    // The engine neither consumed nor altered the tester's fields: they come back as they went in.
    const json after = readSettings(cfg);
    CHECK(after.is_object());
    for (const char* key : {"testerToken", "testerUsageCurrent", "testerUsagePending"}) {
        const bool equal = before.contains(key) && after.contains(key) && before[key] == after[key];
        if (!equal) { std::printf("    FAIL %s came back changed\n", key); }
        CHECK(equal);
    }
    // ...and the ones the seed left out are still the defaults (a file the engine rewrote names them empty; none of them was filled).
    CHECK(!after.value("testerTokenInvalid", false));
    CHECK(after.value("testerAppToken", std::string()).empty());
    CHECK(after.value("testerAppTokenName", std::string()).empty());
    CHECK(after.value("testerToken", std::string()) == kToken);
    // No link-request file was written or claimed by the engine kind.
    CHECK(!fs::exists(g_home / "engine" / "link-request"));
}

// A raw connection to the CAT listener: accepted and closed is a client.
void connectAndClose(int port) {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(2, 0);
    (void)c.Get("/");
}

void testWindowSendsAndMarks() {
    std::printf("  the running window, the control\n");
    StandIn site;
    CHECK(site.start());
    closedPorts();
    emt::setEnv("FOXSDR_TESTER_USAGE_URL", site.base() + "/api/tester-usage");  // the usage endpoint only: the portal stays closed (a refused exchange keeps the code)
    const int webPort = freePort();
    int catPort = freePort();
    while (catPort == webPort) { catPort = freePort(); }
    const fs::path cfg = g_home / "window" / "config.json";
    writeSettings(cfg, webPort, catPort);
    // A script that holds the frame loop in real time, so the two listeners are up and the client below has time to be one.
    const fs::path script = g_home / "window" / "hold.script";
    std::ofstream(script, std::ios::binary | std::ios::trunc) << "6 sleep 5000\n10 sleep 5000\n14 sleep 5000\n";
    emt::setEnv("CASCADE_CONFIG_TEST", cfg.string());
    emt::setEnv("FOXSDR_INPUT_SCRIPT", script.string());
    emt::setEnv("FOXSDR_INSTANCE_SCOPE", "tho-" + std::to_string(::GetCurrentProcessId()));
    emt::setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    emt::setEnv("FOXSDR_FAKE_PACKAGE", "none");

    std::atomic<bool> stop{false};
    std::atomic<int> webAnswer{0};
    std::atomic<bool> catTried{false};
    std::thread client([&] {
        // The listeners come up when the window has restored the settings; ask until the web page answers (every answer but 404 and 401 is a mark), then the CAT port.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (!stop.load() && std::chrono::steady_clock::now() < until) {
            httplib::Client web("127.0.0.1", webPort);
            web.set_connection_timeout(1, 0);
            web.set_read_timeout(2, 0);
            if (auto res = web.Get("/")) {
                webAnswer = res->status;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        while (!stop.load() && std::chrono::steady_clock::now() < until && !catTried.load()) {
            connectAndClose(catPort);
            catTried = true;
        }
    });
    const emt::RunResult r = emt::run(emt::appExe("cascade"), {"--frames", "40"}, g_home / "window", g_home / "window-out", "run", 180000);
    stop = true;
    client.join();
    CHECK(r.started);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 0);
    CHECK(emt::contains(r.out, "rendered 40 frames"));

    // THE STAND-IN HEARD THE LEFT REPORT and the queued one: two POSTs of the usage endpoint, each under the tester's code.
    const std::vector<StandIn::Request> heard = site.requests();
    int posts = 0;
    for (const StandIn::Request& q : heard) {
        std::printf("    the stand-in was asked: %s %s\n", q.method.c_str(), q.path.c_str());
        if (q.method == "POST" && q.path == "/api/tester-usage") { ++posts; }
    }
    CHECK(posts == 2);
    // ...and the queue is empty because the site took them.
    const json after = readSettings(cfg);
    CHECK(after.is_object());
    CHECK(after.value("testerToken", std::string()) == kToken);
    CHECK(after.contains("testerUsagePending") && after["testerUsagePending"].empty());

    // THE MARKS ONLY THE LISTENERS' THREADS CAN MAKE reached the recorder through the door: this session's report, written at exit, names them.
    std::printf("    the web page answered %d, the CAT client tried: %s\n", webAnswer.load(), catTried.load() ? "yes" : "no");
    CHECK(webAnswer.load() != 0 && webAnswer.load() != 404 && webAnswer.load() != 401);
    const json current = json::parse(after.value("testerUsageCurrent", std::string()), nullptr, false);
    CHECK(current.is_object());
    std::set<std::string> features;
    if (current.is_object() && current.contains("features")) {
        for (const auto& f : current["features"]) { features.insert(f.get<std::string>()); }
    }
    for (const std::string& f : features) { std::printf("    this session used: %s\n", f.c_str()); }
    CHECK(features.count("browser") == 1);
    CHECK(features.count("cat") == 1);
    CHECK(features.count("spectrum") == 1);
    CHECK(features.count("left-by-the-last-session") == 0);  // the left report was sent as it was, not folded into this one
}

// A code the site no longer knows: the first send is refused 401, and that is the end of sending for the session - the code is kept (marked, to be fixed in the
// section), nothing is dropped from the queue, and the second queued report is NOT tried. (An "invalid token keeps sending" mutation is a loop against the site.)
void testWindowStopsOnAnInvalidCode() {
    std::printf("  the running window, a code the site refuses\n");
    StandIn site;
    CHECK(site.start(401));
    closedPorts();
    emt::setEnv("FOXSDR_TESTER_USAGE_URL", site.base() + "/api/tester-usage");
    const fs::path cfg = g_home / "invalid" / "config.json";
    writeSettings(cfg, 0, 0);
    const fs::path script = g_home / "invalid" / "hold.script";
    std::ofstream(script, std::ios::binary | std::ios::trunc) << "6 sleep 3000\n10 sleep 3000\n14 sleep 3000\n";
    emt::setEnv("CASCADE_CONFIG_TEST", cfg.string());
    emt::setEnv("FOXSDR_INPUT_SCRIPT", script.string());
    const emt::RunResult r = emt::run(emt::appExe("cascade"), {"--frames", "40"}, g_home / "invalid", g_home / "invalid-out", "run", 180000);
    CHECK(r.started);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 0);
    int posts = 0;
    for (const StandIn::Request& q : site.requests()) {
        std::printf("    the stand-in was asked: %s %s\n", q.method.c_str(), q.path.c_str());
        if (q.method == "POST") { ++posts; }
    }
    CHECK(posts == 1);  // asked once, refused, and not asked again
    const json after = readSettings(cfg);
    CHECK(after.is_object());
    CHECK(after.value("testerToken", std::string()) == kToken);  // kept, for the tester to fix
    CHECK(after.value("testerTokenInvalid", false));            // ...and marked
    CHECK(after.contains("testerUsagePending") && after["testerUsagePending"].size() == 2);  // nothing dropped
}

// A SITE THAT IS DOWN OR BUSY (a 500, as a stand-in for 429 and for a transport failure: all are "retry"): the report is kept, the code is NOT marked invalid, and the
// window asks ONCE for the rest of the session - not on every frame. (Without the session's stop flag the poll, run every frame, is a loop against the site.)
void testWindowBacksOffOnARetry() {
    std::printf("  the running window, a site that is down\n");
    StandIn site;
    CHECK(site.start(500));
    closedPorts();
    emt::setEnv("FOXSDR_TESTER_USAGE_URL", site.base() + "/api/tester-usage");
    const fs::path cfg = g_home / "retry" / "config.json";
    writeSettings(cfg, 0, 0);
    const fs::path script = g_home / "retry" / "hold.script";
    std::ofstream(script, std::ios::binary | std::ios::trunc) << "6 sleep 3000\n10 sleep 3000\n14 sleep 3000\n";
    emt::setEnv("CASCADE_CONFIG_TEST", cfg.string());
    emt::setEnv("FOXSDR_INPUT_SCRIPT", script.string());
    const emt::RunResult r = emt::run(emt::appExe("cascade"), {"--frames", "40"}, g_home / "retry", g_home / "retry-out", "run", 180000);
    CHECK(r.started);
    CHECK(!r.timedOut);
    CHECK(r.exitCode == 0);
    int posts = 0;
    for (const StandIn::Request& q : site.requests()) {
        if (q.method == "POST") { ++posts; }
    }
    std::printf("    the stand-in was asked %d time(s)\n", posts);
    CHECK(posts == 1);
    const json after = readSettings(cfg);
    CHECK(after.is_object());
    CHECK(after.value("testerToken", std::string()) == kToken);
    CHECK(!after.value("testerTokenInvalid", false));  // a busy site is not a refused code
    CHECK(after.contains("testerUsagePending") && after["testerUsagePending"].size() == 2);  // kept for the next launch
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_tester_host_only\n");
    const fs::path root = findRepoRoot(argc > 1 ? argv[1] : std::string());
    CHECK(!root.empty());
    if (root.empty()) { return testSummary("test_tester_host_only"); }

    g_home = fs::temp_directory_path() / ("cascade-tester-host-only-" + std::to_string(
#if defined(_WIN32)
                                                                            static_cast<int>(::GetCurrentProcessId())
#else
                                                                            static_cast<int>(::getpid())
#endif
                                                                                ));
    std::error_code ec;
    fs::remove_all(g_home, ec);
    fs::create_directories(g_home / "tmp", ec);
    emt::isolate(g_home);

    testSources(root);
    testProcessBoot(root);
    testImages();
    testEngineSendsNothing();
    testWindowSendsAndMarks();
    testWindowStopsOnAnInvalidCode();
    testWindowBacksOffOnARetry();

    if (g_checksFailed == 0) { fs::remove_all(g_home, ec); }
    return testSummary("test_tester_host_only");
}
