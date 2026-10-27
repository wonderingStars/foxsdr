/*
 * THE BETA TESTER'S SECTION, IN THE REAL APPLICATION (0.99.44 / 0.99.45, stub campaign B15).
 *
 * The pure halves are pinned elsewhere - the payload, the queue and the sender in tests/test_tester_usage.cpp, the link file and the two network calls in
 * tests/test_tester_link.cpp, the window's decisions about a link in tests/test_tester_link_app.cpp. Nothing held the SECTION: SYSTEM > Beta tester, what it
 * says and does, the "Link this FoxSDR?" prompt, and the record the window keeps while a code is set. The ledger id TESTER-USAGE named a placeholder
 * (test_team_message_app) that tests another feature, so this branch's window lost the section without a red test (plan section 0.3, decision 5). This runs
 * cascade itself, with the application's own input script aimed from the UI census, and checks from the census, the settings file it saved and the diagnostic
 * log it wrote:
 *
 *   hidden        with no code the section is not on the rail, and nothing of a record is kept (the settings file has no report, no token);
 *   reveal        `--link-tester` shows it with nothing held: chip OFF, the paste box, the key;
 *   paste         a portal code typed into the (masked) box and used: chip ON, the code shown four-and-four, the preview carrying the masked token and the
 *                 features the session used (the receiver view; the generator, which the TABLE says), the whole code in the settings file and nowhere else;
 *   invalid       a code the site refused: chip INVALID, the sentence, and the box to paste a current one;
 *   linked        a confirmed link is shown by name with Unlink and no box; Unlink clears it and puts the box back;
 *   prompt        a link-request file is claimed on a worker, the name asked of the site, and the tester is asked: Link stores it (and the section shows it),
 *                 Not now writes nothing; a site that will not answer is said so, once;
 *   events        the plugin store window opening is a "plugins" mark - an event only the window can see.
 *
 * Isolated like every other test that starts the application: its own config, APPDATA, LOCALAPPDATA and diagnostics tree, every FOXSDR_*_URL pointed at a
 * closed local port - except the one scenario that needs a site, which is a stand-in on loopback.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "core/tester_link.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

std::string exePath() {
#if defined(_WIN32)
    return std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    return std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
}

std::string runCmd(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;
    std::string census;  // the whole file, for "the credential is nowhere in it"
    std::string log;
    nlohmann::json config;  // the settings file as the run left it (null when unreadable)
    bool has(const std::string& item) const { return items.count(item) != 0; }
    std::string itemWith(const std::string& prefix) const {
        for (const std::string& it : items) {
            if (it.rfind(prefix, 0) == 0) { return it; }
        }
        return std::string();
    }
    bool anyItemWith(const std::string& prefix) const { return !itemWith(prefix).empty(); }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    // The frame a "-frame" rectangle was last written on; -1 when it never was.
    float frameOf(const std::string& name) const {
        const Rect* r = rect(name);
        return r == nullptr ? -1.0f : r->x0;
    }
    bool logHas(const std::string& needle) const { return log.find(needle) != std::string::npos; }
    std::string cfgString(const char* key) const {
        return config.is_object() && config.contains(key) && config[key].is_string() ? config[key].get<std::string>() : std::string();
    }
};

fs::path g_dir;

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string readTree(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) { all += readFile(it->path()) + "\n"; }
    }
    return all;
}

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

// THE CREDENTIALS THE SCENARIOS USE. A portal code is 32 lowercase hex characters; a link's app token is 40 (the two shapes are never confusable).
const std::string kPortalToken = "0123456789abcdef0123456789abcdef";
const std::string kAppToken = std::string(40, 'a');
const std::string kLinkToken = std::string(40, 'c');
const std::string kMaskedPortal = "0123...cdef";

// THE SITE'S CONFIRM-BY-NAME ENDPOINT (GET /api/beta/app-token/me) on loopback: it answers a name, or refuses, as the scenario says, and records the bearer of
// every lookup so the test can say WHICH token was looked up.
class SiteStandIn {
public:
    bool start(bool answers) {
        answers_ = answers;
        server_.Get("/api/beta/app-token/me", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                bearers_.push_back(req.get_header_value("Authorization"));
            }
            if (answers_) {
                res.status = 200;
                res.set_content("{\"name\":\"Linked Tester\"}", "application/json");
            } else {
                res.status = 500;
                res.set_content("{}", "application/json");
            }
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) { return false; }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
        return true;
    }
    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<std::string> bearers() {
        std::lock_guard<std::mutex> lk(mu_);
        return bearers_;
    }
    ~SiteStandIn() {
        server_.stop();
        if (thread_.joinable()) { thread_.join(); }
    }

private:
    bool answers_ = true;
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> bearers_;
};

struct Run {
    std::string tag;
    std::string configExtra;      // members to add to the settings file, each ending in a comma
    bool openSection = true;      // FOXSDR_OPEN_BETA_TESTER
    bool openPreview = false;     // FOXSDR_OPEN_BETA_TESTER_PREVIEW
    bool linkTesterFlag = false;  // --link-tester
    bool openStore = false;       // FOXSDR_OPEN_PLUGIN_STORE
    bool startReceiver = false;   // CASCADE_DECODE_TEST: a bounded run stays stopped unless it is asked to receive (the built-in generator then runs)
    std::string linkRequest;      // a token written to the link-request file before the run
    std::string siteUrl;          // FOXSDR_BETA_API_URL for this run
    std::string script;
    int frames = 110;
};

Result once(const Run& r) {
    Result out;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path diag = g_dir / (r.tag + "-diag");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    fs::remove(census, ec);
    writeFile(cfg, std::string("{ ") + r.configExtra +
                       "\"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"diagnosticsEnabled\": true, \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", "
                       "\"interfaceScale\": \"100\", \"mainView\": \"receiver\", \"railBank\": 4, \"bandPlanOverlay\": false }\n");
    if (!r.linkRequest.empty()) {
        // The link-request file is written where the run's own poll looks: beside its settings file (the process boot wrote it there for a real click).
        CHECK(cascade::core::writeLinkRequestFile(cfg.parent_path().string(), r.linkRequest));
    }
    const std::string scriptPath = (g_dir / (r.tag + ".script")).string();
    writeFile(scriptPath, r.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", r.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_OPEN_BETA_TESTER", r.openSection ? "1" : "");
    setEnv("FOXSDR_OPEN_BETA_TESTER_PREVIEW", r.openPreview ? "1" : "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", r.openStore ? "1" : "");
    setEnv("CASCADE_DECODE_TEST", r.startReceiver ? "1" : "");
    setEnv("FOXSDR_BETA_API_URL", r.siteUrl.empty() ? std::string("http://127.0.0.1:9/") : r.siteUrl);
    out.out = runCmd("\"" + exePath() + "\"" + (r.linkTesterFlag ? " --link-tester" : "") + " --frames " + std::to_string(r.frames) + " 2>&1");
    out.log = readTree(diag);
    const bool rendered = out.out.find("rendered " + std::to_string(r.frames) + " frames") != std::string::npos;
    const bool written = out.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), out.out.c_str());
        return out;
    }
    out.census = readFile(census);
    std::istringstream in(out.census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { out.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) { tok.push_back(t); }
        if (tok.size() < 5) { continue; }
        const std::size_t n = tok.size();
        Rect rc;
        rc.x0 = std::strtof(tok[n - 4].c_str(), nullptr);
        rc.y0 = std::strtof(tok[n - 3].c_str(), nullptr);
        rc.x1 = std::strtof(tok[n - 2].c_str(), nullptr);
        rc.y1 = std::strtof(tok[n - 1].c_str(), nullptr);
        std::string name = tok[0];
        for (std::size_t i = 1; i + 4 < n; ++i) { name += " " + tok[i]; }
        out.rects[name] = rc;
    }
    out.config = nlohmann::json::parse(readFile(cfg), nullptr, false);
    out.ok = true;
    return out;
}

std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2, frame + 4);
    return buf;
}
std::string typeText(int frame, const std::string& text) { return std::to_string(frame) + " text " + text + "\n"; }

const Rect* need(const Result& r, const char* name) {
    const Rect* rc = r.rect(name);
    if (rc == nullptr) { std::printf("    missing rectangle %s\n", name); }
    return rc;
}

// THE LONGEST preview line the run drew: the census keeps every version of it, and the session's features only ever grow.
nlohmann::json previewOf(const Result& r) {
    std::string best;
    for (const std::string& it : r.items) {
        if (it.rfind("tester:json:", 0) == 0 && it.size() > best.size()) { best = it; }
    }
    if (best.empty()) { return nlohmann::json(); }
    return nlohmann::json::parse(best.substr(std::string("tester:json:").size()), nullptr, false);
}

// The report's shape is toJson()'s: features, plugins and radios are members of the report itself, and the session is {start, minutes}.
bool featureIn(const nlohmann::json& report, const std::string& id) {
    if (!report.is_object() || !report.contains("features")) { return false; }
    for (const auto& f : report["features"]) {
        if (f.is_string() && f.get<std::string>() == id) { return true; }
    }
    return false;
}

// --- THE SCENARIOS ---------------------------------------------------------------------------------------------------------------------------------------

void testHidden() {
    std::printf("  hidden (no code, no flag)\n");
    Run run;
    run.tag = "hidden";
    run.openSection = false;
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    // The bank is the SYSTEM bank (its Usage reporting row is there), and the tester's row is not.
    CHECK(r.anyItemWith("section:Usage reporting"));
    CHECK(!r.anyItemWith("section:Beta tester"));
    CHECK(!r.anyItemWith("tester:chip:"));
    // NO CODE, NOTHING COLLECTED: the settings file the run saved carries no token and no report.
    CHECK(r.cfgString("testerToken").empty());
    CHECK(r.cfgString("testerAppToken").empty());
    CHECK(r.cfgString("testerUsageCurrent").empty());
}

void testReveal(Rect& field, Rect& use) {
    std::printf("  reveal (--link-tester)\n");
    Run run;
    run.tag = "reveal";
    run.linkTesterFlag = true;
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.anyItemWith("section:Beta tester"));
    CHECK(r.has("tester:chip:OFF"));
    CHECK(r.has("tester:open"));
    CHECK(!r.anyItemWith("tester:code:"));
    CHECK(!r.anyItemWith("tester:linked:"));
    for (const char* name : {"tester:field", "tester:use", "tester:preview-key"}) { CHECK(r.rect(name) != nullptr); }
    if (r.rect("tester:field") != nullptr) { field = *r.rect("tester:field"); }
    if (r.rect("tester:use") != nullptr) { use = *r.rect("tester:use"); }
    // Nothing is collected until a code is pasted.
    CHECK(r.cfgString("testerUsageCurrent").empty());
    std::printf("    paste box at %.0f,%.0f, key at %.0f,%.0f\n", field.cx(), field.cy(), use.cx(), use.cy());
}

void testPaste(const Rect& field, const Rect& use) {
    std::printf("  paste a code\n");
    Run run;
    run.tag = "paste";
    run.linkTesterFlag = true;
    run.openPreview = true;
    run.startReceiver = true;
    run.script = click(20, field) + typeText(30, kPortalToken) + click(40, use);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    // ARMED: the chip is ON on the frames after the press, the code is shown four-and-four, and the box is gone.
    CHECK(r.has("tester:chip:ON"));
    CHECK(r.has("tester:code:" + kMaskedPortal));
    CHECK(r.has("tester:preview"));
    CHECK(r.frameOf("tester:code-frame") > 45.0f);
    CHECK(r.frameOf("tester:field-frame") < r.frameOf("tester:code-frame"));
    // WHAT THE PREVIEW SAYS IS SENT: the masked token, this version, and the features the session used since the code went in.
    const nlohmann::json report = previewOf(r);
    CHECK(report.is_object());
    if (report.is_object()) {
        CHECK(report.value("token", std::string()) == kMaskedPortal);
        CHECK(!report.value("version", std::string()).empty());
        CHECK(!report.value("platform", std::string()).empty());
        // The receiver view is drawn every frame (a window event), the built-in generator is the source in use and running (a fact of the table).
        CHECK(featureIn(report, "spectrum"));
        CHECK(featureIn(report, "siggen"));
        // ...and nothing was invented: no radio was opened, no plugin store was.
        CHECK(report.contains("radios") && report["radios"].empty());
        CHECK(!featureIn(report, "plugins"));
        CHECK(!featureIn(report, "transmit"));
    }
    // THE CREDENTIAL IS IN THE SETTINGS FILE (the tester typed it once and should not have to again) AND NOWHERE ELSE: not the census, not the log.
    CHECK(r.cfgString("testerToken") == kPortalToken);
    CHECK(r.census.find(kPortalToken) == std::string::npos);
    CHECK(r.log.find(kPortalToken) == std::string::npos);
    CHECK(r.out.find(kPortalToken) == std::string::npos);
    // The journal wrote this session's report for the next start to send: the whole token in it (it travels), the features in it.
    const std::string current = r.cfgString("testerUsageCurrent");
    CHECK(!current.empty());
    const nlohmann::json written = nlohmann::json::parse(current, nullptr, false);
    CHECK(written.is_object() && written.value("token", std::string()) == kPortalToken);
    CHECK(featureIn(written, "spectrum"));
}

void testInvalid() {
    std::printf("  a code the site refused\n");
    Run run;
    run.tag = "invalid";
    run.configExtra = "\"testerToken\": \"" + kPortalToken + "\", \"testerTokenInvalid\": true, ";
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("tester:chip:INVALID"));
    CHECK(r.has("tester:invalid"));
    CHECK(r.has("tester:code:" + kMaskedPortal));
    // The code is kept for the tester to fix, and the box to paste a current one is there.
    CHECK(r.rect("tester:field") != nullptr);
    CHECK(r.rect("tester:remove") != nullptr);
    CHECK(r.cfgString("testerToken") == kPortalToken);
}

void testLinked() {
    std::printf("  a confirmed link, and Unlink\n");
    const std::string cfg = "\"testerAppToken\": \"" + kAppToken + "\", \"testerAppTokenName\": \"Ada Lovelace\", ";
    Run aim;
    aim.tag = "linked-aim";
    aim.configExtra = cfg;
    aim.linkTesterFlag = true;  // (the section stays after Unlink only while --link-tester shows it: without the flag it goes with the link, below)
    const Result a = once(aim);
    CHECK(a.ok);
    if (!a.ok) { return; }
    // The section is on the rail with nothing but the config to say why (no flag), by name, and the box is not drawn: a link has been confirmed.
    CHECK(a.anyItemWith("section:Beta tester"));
    CHECK(a.has("tester:chip:ON"));
    CHECK(a.has("tester:linked:Ada Lovelace"));
    CHECK(a.rect("tester:unlink") != nullptr);
    CHECK(a.rect("tester:field") == nullptr);
    CHECK(!a.anyItemWith("tester:code:"));
    // It is the link's token the session reports under.
    CHECK(a.cfgString("testerAppToken") == kAppToken);
    const nlohmann::json current = nlohmann::json::parse(a.cfgString("testerUsageCurrent"), nullptr, false);
    CHECK(current.is_object() && current.value("token", std::string()) == kAppToken);
    if (need(a, "tester:unlink") == nullptr) { return; }

    Run run;
    run.tag = "linked";
    run.configExtra = cfg;
    run.linkTesterFlag = true;
    run.script = click(30, *a.rect("tester:unlink"));
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    // Unlink: the line stops being drawn at the press and the box takes its place; the link and what was queued under it are gone from the file.
    CHECK(r.frameOf("tester:linked-frame") < 40.0f);
    CHECK(r.frameOf("tester:field-frame") > 40.0f);
    CHECK(r.cfgString("testerAppToken").empty());
    CHECK(r.cfgString("testerAppTokenName").empty());
    CHECK(r.cfgString("testerUsageCurrent").empty());

    // WITHOUT THE FLAG the section goes with the link: nothing is held, so nothing is shown (testerSectionVisible), and the row is not drawn again after the press.
    Run bare;
    bare.tag = "linked-bare";
    bare.configExtra = cfg;
    bare.script = click(30, *a.rect("tester:unlink"));
    const Result b = once(bare);
    CHECK(b.ok);
    if (b.ok) {
        CHECK(b.frameOf("tester:section-frame") < 40.0f);
        CHECK(b.cfgString("testerAppToken").empty());
    }
}

void testPrompt() {
    std::printf("  the link prompt\n");
    // A link click, as the process boot leaves it: the file in the settings folder. The run claims it on a worker, asks the site whose it is, and asks the tester.
    SiteStandIn site;
    CHECK(site.start(true));
    const std::string hold = "8 sleep 1500\n14 sleep 1500\n20 sleep 1500\n";

    Run aim;
    aim.tag = "prompt-aim";
    aim.linkRequest = kLinkToken;
    aim.siteUrl = site.baseUrl();
    aim.script = hold;
    const Result a = once(aim);
    CHECK(a.ok);
    if (!a.ok) { return; }
    // The lookup went to the site with THE LINK'S token, once.
    const std::vector<std::string> bearers = site.bearers();
    CHECK(bearers.size() == 1);
    CHECK(!bearers.empty() && bearers[0] == "Bearer " + kLinkToken);
    CHECK(a.has("tester:link-prompt"));
    CHECK(a.has("tester:link-prompt:Linked Tester"));
    CHECK(a.has("tester:link-prompt-kind:fresh"));
    // Asked, not decided: nothing is stored until Link is pressed, and the file was claimed (it is gone from the settings folder).
    CHECK(a.cfgString("testerAppToken").empty());
    CHECK(!fs::exists(cascade::core::linkRequestPath((g_dir).string())));
    // The token is a credential: not in the census, not in the log.
    CHECK(a.census.find(kLinkToken) == std::string::npos);
    CHECK(a.log.find(kLinkToken) == std::string::npos);
    if (need(a, "tester:link:yes") == nullptr || need(a, "tester:link:no") == nullptr) { return; }

    // LINK: stored, shown by name, the record armed under it.
    Run link;
    link.tag = "prompt-link";
    link.linkRequest = kLinkToken;
    link.siteUrl = site.baseUrl();
    link.script = hold + click(40, *a.rect("tester:link:yes"));
    const Result l = once(link);
    CHECK(l.ok);
    if (l.ok) {
        CHECK(l.cfgString("testerAppToken") == kLinkToken);
        CHECK(l.cfgString("testerAppTokenName") == "Linked Tester");
        CHECK(l.has("tester:linked:Linked Tester"));
        CHECK(l.has("tester:chip:ON"));
        CHECK(l.log.find(kLinkToken) == std::string::npos);
    }

    // NOT NOW: nothing is written, the section does not appear for it, and the prompt does not come back.
    Run no;
    no.tag = "prompt-no";
    no.linkRequest = kLinkToken;
    no.siteUrl = site.baseUrl();
    no.script = hold + click(40, *a.rect("tester:link:no"));
    const Result n = once(no);
    CHECK(n.ok);
    if (n.ok) {
        CHECK(n.cfgString("testerAppToken").empty());
        CHECK(!n.anyItemWith("tester:linked:"));
    }

    // A SITE THAT WILL NOT ANSWER: the tester is told, once, and nothing is offered as if it were a link.
    SiteStandIn deaf;
    CHECK(deaf.start(false));
    Run fail;
    fail.tag = "prompt-fail";
    fail.linkRequest = kLinkToken;
    fail.siteUrl = deaf.baseUrl();
    fail.script = hold;
    const Result f = once(fail);
    CHECK(f.ok);
    if (f.ok) {
        CHECK(f.has("tester:link-prompt"));
        CHECK(!f.anyItemWith("tester:link-prompt:"));
        CHECK(f.has("tester:link-error:could not check that link - try again"));
        CHECK(f.rect("tester:link:dismiss") != nullptr);
        CHECK(f.cfgString("testerAppToken").empty());
    }
}

void testEvents() {
    std::printf("  events (the plugin store window)\n");
    Run run;
    run.tag = "events";
    run.linkTesterFlag = true;
    run.openPreview = true;
    run.openStore = true;
    // The code goes in through the settings file here: this scenario is about what is marked, not about pasting.
    run.configExtra = "\"testerToken\": \"" + kPortalToken + "\", ";
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    const nlohmann::json report = previewOf(r);
    CHECK(report.is_object());
    CHECK(featureIn(report, "plugins"));
    CHECK(featureIn(report, "spectrum"));
    // The receiver was never started in this run, so the built-in generator was not USED: a fact of the table says what is running, not what is merely selected.
    CHECK(!featureIn(report, "siggen"));
}

}  // namespace

int main() {
    std::printf("test_tester_usage_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-tester-usage-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "home", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL", "FOXSDR_CATALOGUE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    // A private single-instance scope: another FoxSDR on the desktop must not be "the running instance" of these runs (core/tester_link.hpp).
    setEnv("FOXSDR_INSTANCE_SCOPE", "tua-" + std::to_string(pid()));
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_FAKE_PACKAGE", "none");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    Rect field;
    Rect use;
    testHidden();
    testReveal(field, use);
    testPaste(field, use);
    testInvalid();
    testLinked();
    testPrompt();
    testEvents();

    if (g_checksFailed == 0) { fs::remove_all(g_dir, ec); }
    return testSummary("test_tester_usage_app");
}
