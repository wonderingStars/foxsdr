/*
 * THE PLUGIN STORE AND THE FITTED MODULES WINDOW, DRIVEN THROUGH THE KEYS A USER PRESSES
 * (0.99.72, the shop-window redesign). Runs cascade itself, isolated like every test that
 * starts the application, with a LOCAL catalogue and a scripted pointer, and checks what the UI
 * census saw and what the run saved:
 *
 *   browse     the store opens on BROWSE: both tabs are there and BROWSE is the active one; the
 *              UPDATES tab counts the one update the catalogue offers; store-a's card has its
 *              name and a GET key, store-b's (a legal notice) a GET key too, store-c's (older
 *              copy installed) an UPDATE key; the cards sit in their category sections;
 *   page       a click on store-a's name opens its page: SCREENSHOTS, WHAT IT DOES, WHAT'S NEW
 *              and DETAILS in that order down the page, no BEFORE YOU FIT IT (it has no notice),
 *              and the no-pictures frame (the catalogue names none);
 *   notice     GET on store-b's card OPENS ITS PAGE rather than fitting it; BEFORE YOU FIT IT sits
 *              between WHAT'S NEW and DETAILS; the page's key is GREYED until the tick - and after
 *              a scripted click on the tick it is GET;
 *   updates    the UPDATES tab lists store-c with "1.0.0 to 1.1.0" and its UPDATE key;
 *   rectangle  the store window's rectangle is saved by one run and is where the next run opens
 *              it - and a rectangle put in the config by hand is the one the run reports back;
 *   fitted     the Fitted modules window: the five chips and their counts (summing to the one
 *              module fitted), the search, SCAN AGAIN and RESET WINDOW SIZES, store-c's row with
 *              its STOP and REMOVE keys, REMOVE arming a CONFIRM beside it (and nothing deleted),
 *              STOP stopping it; a click on its name opens the module's page with ON THIS MACHINE
 *              first, then the catalogue's SCREENSHOTS, WHAT IT DOES and WHAT'S NEW.
 *
 * Every click is aimed at the rectangle a first, unscripted run of the same layout reported for
 * that key, so nothing here depends on where the window happens to open.
 *
 * NO NETWORK. The catalogue's addresses are https placeholders that are never fetched: the index
 * is read from a file, no page carries a picture, and every telemetry address is a discard port.
 * The plugin folder is a scratch one (FOXSDR_FAKE_PACKAGE puts it under LOCALAPPDATA), holding
 * store-c's older copy: the probe module of tests/fixtures/rescan_probe_plugin.cpp, a real
 * decoder the host really loads, copied in as store-c 1.0.0 with a record in installed.json.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
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

std::string run(const std::string& cmd) {
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
    float w() const { return x1 - x0; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;
    bool has(const std::string& item) const { return items.count(item) != 0; }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
};

fs::path g_dir;
std::string g_probeDll;  // the fixture module copied in as store-c 1.0.0

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }
fs::path indexPath() { return g_dir / "index.json"; }

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

std::string hex64(char c) { return std::string(64, c); }

// THE PICTURES store-a's page is given, with no network: a 2 x 2 PNG written by an encoder outside
// this tree, and a file that is NOT a PNG. Both sit in the plugins folder's store-cache under their
// own sha256, which is what a fetch would have left there - so the page finds them ready ("a cached
// file is returned with no request"), decodes the first to a texture and says why it cannot the
// second.
const unsigned char kTwoByTwoPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x08, 0x1b, 0x18, 0x80, 0xf4,
    0x7f, 0x2e, 0x11, 0x39, 0x06, 0x00, 0x3b, 0x7c, 0x05, 0xb8, 0xfb, 0x73, 0xdc, 0x62, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
const char* const kNotAPng = "this is a text file that says it is a picture";
std::string g_pngSha;
std::string g_junkSha;

std::string sha256Of(const void* data, std::size_t n) {
    std::string hex, err;
    cascade::core::PluginRepo::sha256Hex(data, n, hex, err);
    return hex;
}

// One catalogue entry, for THIS host's os and arch (the only build the store looks for). `extra` is
// more JSON fields (each ending in a comma), put before the home page.
std::string pluginJson(const std::string& id, const std::string& name, const std::string& version,
                       const std::string& category, const std::string& notice, const std::string& whatsNew,
                       const std::string& extra = std::string()) {
    const std::string file = id + "-" + version + kExt;
    std::ostringstream o;
    o << "    {\n"
      << "      \"id\": \"" << id << "\", \"name\": \"" << name << "\", \"version\": \"" << version << "\",\n"
      << "      \"author\": \"Store Test Maker\", \"licence\": \"MIT\", \"abiVersion\": "
      << CASCADE_PLUGIN_ABI_VERSION << ",\n"
      << "      \"capabilities\": [\"CASCADE_CAP_DECODER\"],\n"
      << "      \"summary\": \"The summary of " << name << ", one short line.\",\n"
      << "      \"description\": \"What " << name << " does, in a sentence or two of plain prose.\",\n"
      << "      \"category\": \"" << category << "\", \"experimental\": false,\n"
      << "      \"whatsNew\": \"" << whatsNew << "\", \"published\": \"2026-10-07\",\n";
    if (!notice.empty()) { o << "      \"legalNotice\": \"" << notice << "\",\n"; }
    if (!extra.empty()) { o << "      " << extra << "\n"; }
    o << "      \"homepage\": \"https://example.invalid/" << id << "\",\n"
      << "      \"platforms\": [ { \"os\": \"" << cascade::core::PluginRepo::hostOs() << "\", \"arch\": \""
      << cascade::core::PluginRepo::hostArch() << "\", \"file\": \"" << file
      << "\", \"url\": \"https://example.invalid/" << id << "/" << file << "\", \"sha256\": \""
      << hex64(id.back() == 'a' ? 'a' : (id.back() == 'b' ? 'b' : 'c')) << "\", \"sizeBytes\": 4096 } ]\n"
      << "    }";
    return o.str();
}

// The Fitted modules window opens a stagger slot past the main window's right edge (so that it is
// its own operating-system window); in a single-viewport run that place is outside the only window
// there is, so every run that looks at it is given a rectangle inside the main window.
const char* const kFittedRect =
    ", \"fittedModulesX\": 60, \"fittedModulesY\": 50, \"fittedModulesWidth\": 1400, \"fittedModulesHeight\": 880";

std::string indexText() {
    std::ostringstream o;
    o << "{\n  \"schemaVersion\": 1,\n  \"generated\": \"2026-10-08T00:00:00Z\",\n  \"plugins\": [\n"
      << pluginJson("store-a", "Store Alpha", "1.0.0", "aircraft", "", "1.0.0: first release.",
                    "\"screenshots\": [ { \"url\": \"https://example.invalid/shots/a1.png\", \"sha256\": \"" +
                        g_pngSha + "\", \"caption\": \"A two-pixel picture, decoded.\", \"width\": 2, \"height\": 2 }, "
                        "{ \"url\": \"https://example.invalid/shots/a2.png\", \"sha256\": \"" + g_junkSha +
                        "\", \"caption\": \"A file that is not a picture.\", \"width\": 16, \"height\": 10 } ],")
      << ",\n"
      << pluginJson("store-b", "Store Bravo", "1.0.0", "marine",
                    "Reading these messages may be an offence where you are.", "1.0.0: first release.")
      << ",\n"
      << pluginJson("store-c", "Store Charlie", "1.1.0", "broadcast", "",
                    "1.1.0: follows the control channel through a frequency change.")
      << "\n  ]\n}\n";
    return o.str();
}

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// THE PLUGINS FOLDER AS EVERY RUN FINDS IT: store-c 1.0.0 installed (a real module and its record),
// and - for the runs that look at the Fitted modules page without opening the store - the kept
// catalogue the application loads at start-up.
void resetPluginsFolder(bool keptCatalogue) {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_probeDll, pluginsDir() / (std::string("store-c-1.0.0") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    std::ostringstream m;
    m << "{ \"schemaVersion\": 1, \"plugins\": [ { \"id\": \"store-c\", \"name\": \"Store Charlie\", "
         "\"version\": \"1.0.0\", \"file\": \"store-c-1.0.0"
      << kExt << "\", \"abiVersion\": " << CASCADE_PLUGIN_ABI_VERSION << ", \"installedAt\": 1790000000 } ] }\n";
    writeFile(pluginsDir() / "installed.json", m.str());
    // store-a's two pictures, where a fetch would have left them.
    writeFile(pluginsDir() / "store-cache" / (g_pngSha + ".png"),
              std::string(reinterpret_cast<const char*>(kTwoByTwoPng), sizeof kTwoByTwoPng));
    writeFile(pluginsDir() / "store-cache" / (g_junkSha + ".png"), kNotAPng);
    if (keptCatalogue) {
        writeFile(pluginsDir() / "catalogue.json", indexText());
        writeFile(pluginsDir() / "catalogue.json.time", "1790000000\n");
    }
}

int jsonInt(const std::string& text, const char* key) {
    const std::size_t at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) { return -999999; }
    const std::size_t colon = text.find(':', at);
    if (colon == std::string::npos) { return -999999; }
    return std::atoi(text.c_str() + colon + 1);
}

struct RunOptions {
    int frames = 60;
    std::string script;       // empty = no scripted input
    bool openStore = true;    // FOXSDR_OPEN_PLUGIN_STORE
    bool openFitted = false;  // FOXSDR_OPEN_FITTED_MODULES
    std::string extraJson;    // more config JSON (a leading comma), "" = none
    bool keepConfig = false;  // keep the config the previous run with this tag wrote
};

// One run of the application.
Result once(const std::string& tag, const RunOptions& o) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    if (!o.keepConfig) {
        std::ostringstream c;
        c << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
             "\"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": \"receiver\", "
             "\"railBank\": 2, \"bandPlanOverlay\": false, \"pluginCatalogueUrl\": \""
          << indexPath().generic_string() << "\"" << o.extraJson << " }\n";
        writeFile(cfg, c.str());
    }
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, o.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_INPUT_SCRIPT", o.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", o.openStore ? "1" : "");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", o.openFitted ? "1" : "");
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(o.frames) + " 2>&1");
    const bool rendered = r.out.find("rendered " + std::to_string(o.frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string name;
        Rect rc;
        ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
        r.rects[name] = rc;
    }
    r.ok = true;
    return r;
}

std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2,
                  frame + 4);
    return buf;
}

// The rectangle `name` from a layout run, or an empty one (and a FAIL) when it was not drawn.
Rect need(const Result& r, const std::string& name) {
    const Rect* p = r.rect(name);
    CHECK(p != nullptr);
    if (p == nullptr) { std::printf("    missing rect %s\n", name.c_str()); }
    return p != nullptr ? *p : Rect{};
}

void printPageOrder(const Result& r, const char* prefix) {
    for (const char* s : {"onthismachine", "screenshots", "whatitdoes", "whatsnew", "beforeyoufitit", "details"}) {
        const Rect* p = r.rect(std::string(prefix) + ":section:" + s);
        if (p != nullptr) { std::printf("    %s section %-15s y %.0f..%.0f\n", prefix, s, p->y0, p->y1); }
    }
}

// The sections of a page, top to bottom: each present one is below the one before it.
bool sectionsInOrder(const Result& r, const char* prefix, const std::vector<std::string>& names) {
    float last = -1.0e9f;
    for (const std::string& n : names) {
        const Rect* p = r.rect(std::string(prefix) + ":section:" + n);
        if (p == nullptr) { return false; }
        if (!(p->y0 >= last)) { return false; }
        last = p->y1 - 0.01f;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_store_app\n");
    if (argc < 2) {
        std::printf("usage: test_plugin_store_app <probe module>\n");
        return 2;
    }
    g_probeDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-store-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "diag", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (g_dir / "diag").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    // A fake package identity puts the plugins folder under LOCALAPPDATA (the scratch tree), not
    // beside the executable in the build tree.
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    g_pngSha = sha256Of(kTwoByTwoPng, sizeof kTwoByTwoPng);
    g_junkSha = sha256Of(kNotAPng, std::strlen(kNotAPng));
    writeFile(indexPath(), indexText());

    // --- BROWSE ---------------------------------------------------------------
    Rect nameA, keyB, tabUpdates;
    {
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 80;
        const Result r = once("browse", o);
        CHECK(r.ok);
        if (r.ok) {
            // The two tabs, BROWSE active, and the one update counted.
            CHECK(r.rect("store:tab:browse") != nullptr);
            CHECK(r.rect("store:tab:updates") != nullptr);
            CHECK(r.has("store:tab:browse"));
            CHECK(!r.has("store:tab:updates"));
            CHECK(r.has("store:count:updates:1"));
            CHECK(r.rect("store:search") != nullptr);
            CHECK(r.rect("store:check") != nullptr);
            // The tabs sit left to right, BROWSE first, and the search before them.
            if (r.rect("store:tab:browse") != nullptr && r.rect("store:tab:updates") != nullptr &&
                r.rect("store:search") != nullptr) {
                CHECK(r.rect("store:search")->x1 <= r.rect("store:tab:browse")->x0);
                CHECK(r.rect("store:tab:browse")->x1 <= r.rect("store:tab:updates")->x0);
                CHECK(r.rect("store:tab:updates")->x1 <= r.rect("store:check")->x0);
            }
            // THE CATALOGUE WAS READ, so the top bar says so (no kept copy was in use).
            CHECK(!r.has("store:catalogue:cache"));
            // store-a: a card with its name, and GET.
            CHECK(r.rect("store:card:store-a") != nullptr);
            CHECK(r.rect("store:card:store-a:key") != nullptr);
            CHECK(r.rect("store:card:store-a:name") != nullptr);
            CHECK(r.has("store:key:store-a:get"));
            if (r.rect("store:card:store-a:name") != nullptr) { CHECK(r.rect("store:card:store-a:name")->w() > 20.0f); }
            // store-b carries a legal notice: its GET is not greyed (it opens the page).
            CHECK(r.has("store:key:store-b:get"));
            // store-c has an older copy installed: UPDATE.
            CHECK(r.has("store:key:store-c:update"));
            // The categories: aircraft, marine, voice and data - and nothing else.
            CHECK(r.has("store:section:aircraft"));
            CHECK(r.has("store:section:marine"));
            CHECK(r.has("store:section:voice"));
            CHECK(!r.has("store:section:other"));
            // The card sits inside the store window, below the top bar.
            if (r.rect("store:card:store-a") != nullptr && r.rect("store:window") != nullptr) {
                const Rect& c = *r.rect("store:card:store-a");
                const Rect& w = *r.rect("store:window");
                CHECK(c.x0 >= w.x0 && c.x1 <= w.x1 && c.y0 >= w.y0 && c.y1 <= w.y1);
                CHECK(r.rect("store:search") != nullptr && c.y0 > r.rect("store:search")->y1);
            }
            nameA = need(r, "store:card:store-a:name");
            keyB = need(r, "store:card:store-b:key");
            tabUpdates = need(r, "store:tab:updates");
            // GET EVERYTHING sits at the foot, below the last card.
            CHECK(r.rect("store:geteverything") != nullptr);
            if (r.rect("store:geteverything") != nullptr && r.rect("store:card:store-c") != nullptr) {
                CHECK(r.rect("store:geteverything")->y0 >= r.rect("store:card:store-c")->y1);
            }
        }
    }

    // --- A PAGE ---------------------------------------------------------------
    Rect backA, detailsA;
    {
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 90;
        o.script = click(40, nameA);
        const Result r = once("page-a", o);
        CHECK(r.ok);
        if (r.ok) {
            printPageOrder(r, "store");
            CHECK(r.has("store:page:store-a"));
            // SCREENSHOTS, WHAT IT DOES, WHAT'S NEW, DETAILS - in that order down the page - and no
            // BEFORE YOU FIT IT: store-a has no legal notice.
            CHECK(sectionsInOrder(r, "store", {"screenshots", "whatitdoes", "whatsnew", "details"}));
            CHECK(r.rect("store:section:beforeyoufitit") == nullptr);
            // THE PICTURES: the catalogue names two; the first is decoded to a texture (the cached file
            // is a real PNG), the second is a file that is not one and says why instead.
            CHECK(r.has("store:pictures:store-a:2"));
            CHECK(r.has("store:picture:store-a:0:texture"));
            CHECK(r.has("store:picture:store-a:1:error"));
            // (The census holds every frame's notes, so the first frames, before the decode that
            // runs one picture per frame, also noted "loading": what is asserted is that the
            // texture state and the error state were REACHED.)
            CHECK(r.rect("store:page:back") != nullptr);
            CHECK(r.rect("store:page:key") != nullptr);
            CHECK(r.rect("store:page:tick") == nullptr);
            // Not fitted yet, no notice: its key is a plain GET.
            CHECK(r.has("store:page:key:get"));
            // ...and it did not close by itself.
            CHECK(!r.has("store:page:closed:esc"));
            CHECK(!r.has("store:page:closed:back"));
            backA = need(r, "store:page:back");
            detailsA = need(r, "store:page:details");
            CHECK(!r.has("store:details:open"));  // folded until SHOW DETAILS is pressed
        }
    }
    {
        // SHOW DETAILS opens the facts.
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 100;
        o.script = click(40, nameA) + click(80, detailsA);
        const Result r = once("page-a-details", o);
        CHECK(r.ok);
        if (r.ok) { CHECK(r.has("store:details:open")); }
    }
    {
        // ESC RETURNS FROM A PAGE, and so does "< BROWSE": the grid is back either way.
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 100;
        o.script = click(40, nameA) + "80 key escape\n";
        const Result r = once("page-a-esc", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("store:page:store-a"));
            CHECK(r.has("store:page:closed:esc"));
            CHECK(!r.has("store:page:closed:back"));
        }
        resetPluginsFolder(false);
        RunOptions b;
        b.frames = 100;
        b.script = click(40, nameA) + click(80, backA);
        const Result rb = once("page-a-back", b);
        CHECK(rb.ok);
        if (rb.ok) {
            CHECK(rb.has("store:page:closed:back"));
            CHECK(!rb.has("store:page:closed:esc"));
        }
    }

    // --- THE LEGAL NOTICE: GET opens the page, and the key is greyed until the tick ------------
    Rect tick;
    {
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 100;
        o.script = click(40, keyB);
        const Result r = once("page-b", o);
        CHECK(r.ok);
        if (r.ok) {
            printPageOrder(r, "store");
            // GET on a card with a notice did NOT fit it: the page is open, and nothing is being fetched.
            CHECK(r.has("store:page:store-b"));
            CHECK(!r.has("store:key:store-b:fitting"));
            // The catalogue names no pictures for store-b: the frame in the theme, "No pictures published yet".
            CHECK(r.has("store:pictures:store-b:0"));
            CHECK(sectionsInOrder(r, "store", {"screenshots", "whatitdoes", "whatsnew", "beforeyoufitit", "details"}));
            // GREYED BEFORE THE TICK, and never GET on this page.
            CHECK(r.has("store:page:key:greyed"));
            CHECK(!r.has("store:page:key:get"));
            tick = need(r, "store:page:tick");
        }
    }
    {
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 130;
        o.script = click(40, keyB) + click(80, tick);
        const Result r = once("page-b-tick", o);
        CHECK(r.ok);
        if (r.ok) {
            // The page was greyed, then the tick made the key GET.
            CHECK(r.has("store:page:key:greyed"));
            CHECK(r.has("store:page:key:get"));
            // ...and the tick fitted nothing by itself.
            CHECK(!r.has("store:key:store-b:fitting"));
        }
    }

    // --- THE UPDATES TAB ------------------------------------------------------
    {
        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 90;
        o.script = click(40, tabUpdates);
        const Result r = once("updates", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("store:tab:updates"));
            CHECK(r.has("store:update:store-c:1.0.0 to 1.1.0"));
            CHECK(r.has("store:count:updates:1"));
            CHECK(r.rect("store:updateall") != nullptr);
            CHECK(r.rect("store:card:store-c:key") != nullptr);
            // Only the update is a row here: the other two are not on this tab.
            CHECK(!r.has("store:update:store-a:1.0.0 to 1.0.0"));
        }
    }

    // --- THE WINDOW REMEMBERS WHERE IT WAS ---------------------------------------------
    //
    // THE RECTANGLES HERE ARE CHOSEN TO TEST THE REMEMBERING, NOT THE CLAMPING. With nothing saved
    // the store opens at its preferred 1480 x 980 held inside the MAIN window (pageOpenInside;
    // this test's window is 1600 x 1000), and reads back about 1430 wide. A SAVED rectangle is
    // held on the way back in to the MONITOR's work area instead, measured from its own corner
    // (mapClampRestoredSize). On a screen narrower than the main window - the Linux runners' Xvfb
    // screen is 1280 wide - the 1430 px rectangle the first run saves at x 85 is cut to 1195 (1280
    // less 85) on reopening, and the next run saves 1145: CI run 37776378832 failed `w2 - w1 <= 60`
    // that way on both Linux jobs, while this desktop's wider screen cut nothing. So the run whose
    // rectangle is remembered is GIVEN one by hand that fits any plausible screen and the main
    // window (100,80 1100 x 700), and what is asked is that the next run reopens it where the first
    // left it. The unconfigured run below is kept only to know the width the default has.
    {
        resetPluginsFolder(false);
        RunOptions plain;
        plain.frames = 70;
        const Result unconfigured = once("rect-default", plain);
        CHECK(unconfigured.ok);
        const int wDefault = jsonInt(readFile(g_dir / "rect-default.json"), "pluginStoreWidth");
        std::printf("    an unconfigured run saved the store %d wide\n", wDefault);
        CHECK(wDefault > 400);

        resetPluginsFolder(false);
        RunOptions o;
        o.frames = 70;
        o.extraJson =
            ", \"pluginStoreX\": 100, \"pluginStoreY\": 80, \"pluginStoreWidth\": 1100, \"pluginStoreHeight\": 700";
        const Result first = once("rect-1", o);
        CHECK(first.ok);
        const std::string cfgText = readFile(g_dir / "rect-1.json");
        const int w1 = jsonInt(cfgText, "pluginStoreWidth");
        const int h1 = jsonInt(cfgText, "pluginStoreHeight");
        const int x1 = jsonInt(cfgText, "pluginStoreX");
        const int y1 = jsonInt(cfgText, "pluginStoreY");
        std::printf("    run 1 saved the store at %d,%d  %d x %d\n", x1, y1, w1, h1);
        // A run that opened the store saves its rectangle.
        CHECK(w1 > 400);
        CHECK(h1 > 300);
        // ...and it is not the default's: were it the default's, the comparison below would pass
        // whether or not anything had been remembered.
        CHECK(std::abs(w1 - wDefault) > 100);
        // THE NEXT RUN, from the config the first wrote, opens it where it was. The two runs' rectangles
        // are not asked to be EQUAL, because this headless single-viewport run reads a window's position
        // and size back about 25 px in and 50 px smaller than it was set - the Fitted modules window,
        // whose logic this copies, does exactly the same (build/scratch_rect_probe.py, measured 0.99.72:
        // 120,90 1010x710 comes back 145,115 960x660, then 170,140 910x610) - so equality is asked only
        // of the drift being no more than that one step.
        resetPluginsFolder(false);
        RunOptions again;
        again.frames = 70;
        again.keepConfig = true;
        const Result second = once("rect-1", again);
        CHECK(second.ok);
        const std::string cfg2 = readFile(g_dir / "rect-1.json");
        const int w2 = jsonInt(cfg2, "pluginStoreWidth");
        const int x2 = jsonInt(cfg2, "pluginStoreX");
        std::printf("    run 2 saved the store at %d,%d  %d x %d\n", x2, jsonInt(cfg2, "pluginStoreY"), w2,
                    jsonInt(cfg2, "pluginStoreHeight"));
        CHECK(std::abs(x2 - x1) <= 30);
        CHECK(std::abs(jsonInt(cfg2, "pluginStoreY") - y1) <= 30);
        CHECK(std::abs(w2 - w1) <= 60);
        CHECK(std::abs(jsonInt(cfg2, "pluginStoreHeight") - h1) <= 60);
        // AND A RECTANGLE PUT THERE BY HAND IS WHERE THE RUN OPENS IT - the proof that it was restored,
        // and not merely that the default is the default: the default is about 1430 wide (wDefault),
        // and what comes back is the hand-made 1010 x 710 within that one step.
        resetPluginsFolder(false);
        RunOptions hand;
        hand.frames = 70;
        hand.extraJson =
            ", \"pluginStoreX\": 120, \"pluginStoreY\": 90, \"pluginStoreWidth\": 1010, \"pluginStoreHeight\": 710";
        const Result third = once("rect-hand", hand);
        CHECK(third.ok);
        const std::string cfg3 = readFile(g_dir / "rect-hand.json");
        std::printf("    run 3 (rectangle given by hand) saved %d,%d  %d x %d\n", jsonInt(cfg3, "pluginStoreX"),
                    jsonInt(cfg3, "pluginStoreY"), jsonInt(cfg3, "pluginStoreWidth"),
                    jsonInt(cfg3, "pluginStoreHeight"));
        CHECK(std::abs(jsonInt(cfg3, "pluginStoreX") - 120) <= 30);
        CHECK(std::abs(jsonInt(cfg3, "pluginStoreY") - 90) <= 30);
        CHECK(std::abs(jsonInt(cfg3, "pluginStoreWidth") - 1010) <= 60);
        CHECK(std::abs(jsonInt(cfg3, "pluginStoreHeight") - 710) <= 60);
        // ...and it is a different rectangle from the one an unconfigured run opens.
        CHECK(std::abs(jsonInt(cfg3, "pluginStoreWidth") - wDefault) > 100);
    }

    // --- THE FITTED MODULES WINDOW ----------------------------------------------------
    Rect fName, fRemove, fKey, fBack;
    {
        resetPluginsFolder(true);
        RunOptions o;
        o.frames = 80;
        o.openStore = false;
        o.openFitted = true;
        o.extraJson = kFittedRect;
        const Result r = once("fitted", o);
        CHECK(r.ok);
        if (r.ok) {
            // The search, the five chips with their counts, and the two keys at the right.
            CHECK(r.rect("fitted:search") != nullptr);
            CHECK(r.rect("fitted:scan") != nullptr);
            CHECK(r.rect("fitted:reset") != nullptr);
            int counted = 0;
            for (const char* st : {"fed", "notfed", "nosignal", "stopped", "refused"}) {
                CHECK(r.rect(std::string("fitted:chip:") + st) != nullptr);
                for (int n = 0; n <= 3; ++n) {
                    if (r.has(std::string("fitted:count:") + st + ":" + std::to_string(n))) { counted += n; }
                }
            }
            // ONE MODULE IS FITTED, and the five counts add up to it.
            CHECK(counted == 1);
            if (r.rect("fitted:search") != nullptr && r.rect("fitted:chip:fed") != nullptr &&
                r.rect("fitted:scan") != nullptr && r.rect("fitted:reset") != nullptr) {
                CHECK(r.rect("fitted:search")->x1 <= r.rect("fitted:chip:fed")->x0 ||
                      r.rect("fitted:search")->y1 <= r.rect("fitted:chip:fed")->y0);
                // SCAN AGAIN, then RESET WINDOW SIZES at the far right (the mock-up's order).
                CHECK(r.rect("fitted:scan")->x1 <= r.rect("fitted:reset")->x0);
            }
            // store-c's row, with its name, STOP (the key) and REMOVE.
            CHECK(r.rect("fitted:row:store-c") != nullptr);
            CHECK(r.rect("fitted:row:store-c:name") != nullptr);
            CHECK(r.rect("fitted:row:store-c:key") != nullptr);
            CHECK(r.rect("fitted:row:store-c:remove") != nullptr);
            if (r.rect("fitted:row:store-c:key") != nullptr && r.rect("fitted:row:store-c:remove") != nullptr) {
                CHECK(r.rect("fitted:row:store-c:key")->x1 <= r.rect("fitted:row:store-c:remove")->x0);
            }
            // No CONFIRM until REMOVE is pressed.
            CHECK(r.rect("fitted:row:store-c:confirm") == nullptr);
            fName = need(r, "fitted:row:store-c:name");
            fRemove = need(r, "fitted:row:store-c:remove");
            fKey = need(r, "fitted:row:store-c:key");
        }
    }
    {
        // REMOVE ARMS A CONFIRM BESIDE IT, and deletes nothing.
        resetPluginsFolder(true);
        RunOptions o;
        o.frames = 90;
        o.openStore = false;
        o.openFitted = true;
        o.extraJson = kFittedRect;
        o.script = click(40, fRemove);
        const Result r = once("fitted-remove", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.rect("fitted:row:store-c:confirm") != nullptr);
            if (r.rect("fitted:row:store-c:confirm") != nullptr && r.rect("fitted:row:store-c:remove") != nullptr) {
                CHECK(r.rect("fitted:row:store-c:confirm")->x1 <= r.rect("fitted:row:store-c:remove")->x0);
            }
        }
        // The file is still there: the first press only armed it.
        CHECK(fs::exists(pluginsDir() / (std::string("store-c-1.0.0") + kExt)));
    }
    {
        // STOP stops it: the row's state word becomes STOPPED BY YOU.
        resetPluginsFolder(true);
        RunOptions o;
        o.frames = 100;
        o.openStore = false;
        o.openFitted = true;
        o.extraJson = kFittedRect;
        o.script = click(40, fKey);
        const Result r = once("fitted-stop", o);
        CHECK(r.ok);
        if (r.ok) { CHECK(r.has("fitted:state:store-c:stopped")); }
    }
    {
        // A CLICK ON THE NAME OPENS THE PAGE: ON THIS MACHINE first, then the catalogue's sections.
        resetPluginsFolder(true);
        RunOptions o;
        o.frames = 100;
        o.openStore = false;
        o.openFitted = true;
        o.extraJson = kFittedRect;
        o.script = click(40, fName);
        const Result r = once("fitted-page", o);
        CHECK(r.ok);
        if (r.ok) {
            printPageOrder(r, "fitted");
            CHECK(r.has("fitted:page:store-c"));
            CHECK(sectionsInOrder(r, "fitted",
                                  {"onthismachine", "screenshots", "whatitdoes", "whatsnew", "details"}));
            CHECK(r.rect("fitted:page:key") != nullptr);  // the STOP key in the header
            CHECK(r.has("fitted:pictures:store-c:0"));
            CHECK(!r.has("fitted:page:closed:esc"));
            fBack = need(r, "fitted:page:back");
        }
    }
    {
        // ESC AND "< FITTED" BOTH RETURN TO THE LIST.
        resetPluginsFolder(true);
        RunOptions o;
        o.frames = 100;
        o.openStore = false;
        o.openFitted = true;
        o.extraJson = kFittedRect;
        o.script = click(40, fName) + "80 key escape\n";
        const Result r = once("fitted-page-esc", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("fitted:page:store-c"));
            CHECK(r.has("fitted:page:closed:esc"));
        }
        resetPluginsFolder(true);
        RunOptions b = o;
        b.script = click(40, fName) + click(80, fBack);
        const Result rb = once("fitted-page-back", b);
        CHECK(rb.ok);
        if (rb.ok) {
            CHECK(rb.has("fitted:page:closed:back"));
            CHECK(!rb.has("fitted:page:closed:esc"));
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_plugin_store_app");
}
