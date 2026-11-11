// THE SCRIPTED INPUT'S DRIVEN RUNS (src/slint/input_script.hpp; docs/engine-stage4.md 6.47): foxsdr-slint itself, on a scratch profile
// with a scripted pointer and keyboard, every step read back from the engine.
//
//   no argument   the first real driven run: the receiver stopped and started from the strip's key, a bookmark typed and added from the
//                 TOOLS bank, each effect read back through the table (running, count.bookmarks, the list's names) - the proof that a
//                 script reaches a real control and the engine answers;
//   <script>      one script of the drive suite (tests/drive/*.txt, registered one test each by src/slint/CMakeLists.txt): its "#!" header
//                 names the run (frames=, bank=, sections=, view=, open=, store= and any FOXSDR_* variable), the window plays it, and
//                 the run must say every step ran and none failed.
//
// Isolated like every test that starts the application (tests/test_bookmark_tick_app.cpp): APPDATA and the rest point into a scratch folder
// with this process's id in its name, CASCADE_CONFIG_TEST names the run's settings file, every network endpoint is a dead port. Runs on the
// hidden desktop under the gui label (it opens a window).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

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
    if (value.empty()) { ::unsetenv(name); } else { ::setenv(name, value.c_str(), 1); }
#endif
}

// The run's output and its exit code.
struct Run {
    std::string out;
    int exitCode = -1;
};

Run run(const std::string& cmd) {
    Run r;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[1024];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { r.out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { r.exitCode = _pclose(p); }
#else
    if (p != nullptr) { const int st = pclose(p); r.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
#endif
    return r;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The "#!" header lines of a script: key=value tokens. Known keys name the run; an upper-case key is an environment variable.
struct Header {
    int frames = 240;
    std::string plugins;   // "all", or a comma list of plugin ids (file-name prefixes), to fit from the user's plugin folder
    std::string iqfile;    // an I/Q WAV played as the receiver's source instead of the generator (docs 6.50: the fixture runs); a relative path is under tests/drive/fixtures
    std::string centre;    // the source's centre frequency in Hz (the tune at start), with iqfile
    std::string mode;      // the demodulation mode at start (NFM WFM AM DSB USB CW LSB RAW), with iqfile
    std::string bandwidth; // the passband in Hz at start, with iqfile
    std::string config;    // a JSON file used as the run's settings seed instead of the generator profile (docs 6.51: a real radio's profile); relative = under tests/drive/fixtures
    bool userRadios = false;  // radios=user: the user's own SoapySDR vendor modules stay reachable (docs 6.51) - a hardware run on a real radio
    bool updateCheck = false; // updatecheck=on: the generator profile's "Check for updates at startup" tick is ON (it is off otherwise: a bounded run asks nobody)
    std::vector<std::string> logContains;   // logcontains=<text, underscores for spaces>: the run's diagnostic log must hold the text afterwards (docs 6.63: "a refused open reaches the log")
    std::string manifest;     // manifest=<file>: foxsdr.com's answer, canned (FOXSDR_FAKE_UPDATE_MANIFEST, core/updater.hpp); relative = under tests/drive/fixtures (docs 6.63)
    std::map<std::string, std::string> env;
};

Header parseHeader(const std::string& script) {
    Header h;
    std::istringstream in(script);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("#!", 0) != 0) { continue; }
        std::istringstream toks(line.substr(2));
        std::string tok;
        while (toks >> tok) {
            const std::size_t eq = tok.find('=');
            if (eq == std::string::npos) { continue; }
            const std::string key = tok.substr(0, eq), value = tok.substr(eq + 1);
            if (key == "frames") { h.frames = std::atoi(value.c_str()); }
            else if (key == "bank") { h.env["FOXSDR_SLINT_BANK"] = value; }
            else if (key == "sections") { h.env["FOXSDR_SLINT_SECTIONS"] = value; }
            else if (key == "view") { h.env["FOXSDR_SLINT_VIEW"] = value; }
            else if (key == "open") { h.env["FOXSDR_SLINT_OPEN"] = value; }
            else if (key == "store") { h.env["FOXSDR_SLINT_STORE_PAGE"] = value; }
            else if (key == "testradios") { h.env["FOXSDR_SLINT_TEST_RADIOS"] = value; }   // 2..8 made-up radios for the radio workspace (docs/multi-radio-records/screen-s3.md); a test-only seam of a bounded run
            else if (key == "size") { h.env["FOXSDR_SLINT_SIZE"] = value; }   // <w>x<h>: a taller window for a tall section body
            else if (key == "plugins") { h.plugins = value; }                 // all | <id>,<id>...: the plugins fitted for this run (docs 6.49)
            else if (key == "iqfile") { h.iqfile = value; }                   // the I/Q WAV source (docs 6.50)
            else if (key == "centre") { h.centre = value; }
            else if (key == "mode") { h.mode = value; }
            else if (key == "bandwidth") { h.bandwidth = value; }
            else if (key == "config") { h.config = value; }                   // the settings seed file (docs 6.51)
            else if (key == "radios") { h.userRadios = (value == "user"); }   // the user's SoapySDR modules (docs 6.51: a hardware run)
            else if (key == "updatecheck") { h.updateCheck = (value == "on"); }   // the Updates tick on (docs 6.63: the kill switch's notice)
            else if (key == "manifest") { h.manifest = value; }                   // the canned update answer (docs 6.63)
            else if (key == "logcontains") {                                      // a line the run must have written to its diagnostic log (docs 6.63)
                std::string text = value;
                std::replace(text.begin(), text.end(), '_', ' ');
                h.logContains.push_back(text);
            }
            else if (key.rfind("FOXSDR_", 0) == 0 || key.rfind("CASCADE_", 0) == 0) { h.env[key] = value; }
        }
    }
    return h;
}

// The first real driven run (no argument).
const char* kBuiltIn =
    "#! frames=240 bank=2 sections=2\n"
    "# the TOOLS bank with its Bookmarks body open; the generator source\n"
    "10 dump\n"
    "state running == 1\n"
    "state deviceName == \"Signal generator\"\n"
    "state count.bookmarks == 0\n"
    "20 click \"Stop the receiver (Ctrl+F2)\"\n"
    "+15 state running == 0\n"
    "expect \"Start the receiver (Ctrl+F2)\" enabled equals \"true\"\n"
    "+1 click \"Start the receiver (Ctrl+F2)\"\n"
    "+25 state running == 1\n"
    "+1 type \"Bookmark name\" \"Probe\"\n"
    "+2 expect \"Bookmark name\" value equals \"Probe\"\n"
    "+1 click \"Add current\"\n"
    "+25 state count.bookmarks == 1\n"
    "state list.bookmarks.names contains Probe\n"
    "# a dropdown: the receiver strip's MODE key is labelled by its value; the popup row \"AM\" is clicked and the engine's mode read back\n"
    "+1 state mode == WFM\n"
    "+1 choose \"WFM\" \"AM\"\n"
    "+15 state mode == AM\n"
    "expect \"AM\" value equals \"AM\"\n"
    "+1 state cfg.uiTheme == today\n"
    "+1 shot \"drive-run.png\"\n";

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_slint_drive_run\n");
    std::string script = kBuiltIn;
    std::string tag = "builtin";
    if (argc > 1) {
        script = readFile(argv[1]);
        tag = fs::path(argv[1]).stem().string();
        if (script.empty()) {
            std::printf("  cannot read the script %s\n", argv[1]);
            CHECK(!script.empty());
            return testSummary("test_slint_drive_run");
        }
    }
    // {fixtures} in a script or its header is the fixtures folder (forward slashes, which Windows takes as well as backslashes and which need no escaping inside a quoted
    // step): a step can name a recording the same on every machine (docs 6.63). FOXSDR_DRIVE_FIXTURES_DIR names another folder.
    {
        const char* fx = std::getenv("FOXSDR_DRIVE_FIXTURES_DIR");
        const std::string dirText = ((fx != nullptr && *fx != '\0') ? fs::path(fx) : fs::path(FOXSDR_DRIVE_FIXTURES_DIR)).generic_string();
        const std::string token = "{fixtures}";
        for (std::size_t at = script.find(token); at != std::string::npos; at = script.find(token, at + dirText.size())) { script.replace(at, token.size(), dirText); }
    }
    const Header h = parseHeader(script);
    // {FIXTURES} in a script is the fixtures folder, forward slashes (a file the script types into a path box: docs 6.61's airband-import.csv).
    {
        const char* fx = std::getenv("FOXSDR_DRIVE_FIXTURES_DIR");
        const std::string fixtures = fs::path((fx != nullptr && *fx != 0) ? fx : FOXSDR_DRIVE_FIXTURES_DIR).generic_string();
        for (std::size_t at = script.find("{FIXTURES}"); at != std::string::npos; at = script.find("{FIXTURES}", at + fixtures.size())) { script.replace(at, 10, fixtures); }
    }

    const fs::path dir = fs::temp_directory_path() / ("foxsdr-slint-drive-" + tag + "-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "local", ec);
    // THE PLUGINS (docs 6.49): a run that says `plugins=` gets the DLLs copied from the user's own plugin folder (read-only; FOXSDR_DRIVE_PLUGINS_DIR
    // names another) into the scratch profile's %LOCALAPPDATA%\foxsdr\plugins, with catalogue.json and installed.json beside them, BEFORE the
    // variables are redirected - the host scans exactly that folder (core/plugin_host.cpp userPluginDir).
    int pluginsCopied = 0;
    if (!h.plugins.empty()) {
        fs::path from;
        if (const char* o = std::getenv("FOXSDR_DRIVE_PLUGINS_DIR"); o != nullptr && *o != '\0') { from = o; }
        else if (const char* la = std::getenv("LOCALAPPDATA"); la != nullptr && *la != '\0') { from = fs::path(la) / "FoxSDR" / "plugins"; }
        const fs::path to = dir / "local" / "foxsdr" / "plugins";
        fs::create_directories(to, ec);
        std::vector<std::string> wanted;
        if (h.plugins != "all") {
            std::istringstream ids(h.plugins);
            std::string id;
            while (std::getline(ids, id, ',')) { if (!id.empty()) { wanted.push_back(id); } }
        }
        for (const auto& e : fs::directory_iterator(from, ec)) {
            const std::string name = e.path().filename().string();
            const bool dll = name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0;
            const bool meta = name == "catalogue.json" || name == "installed.json";
            if (!dll && !meta) { continue; }
            if (dll && !wanted.empty()) {
                bool hit = false;
                for (const auto& w : wanted) { if (name.rfind(w + "-", 0) == 0) { hit = true; } }
                if (!hit) { continue; }
            }
            fs::copy_file(e.path(), to / name, fs::copy_options::overwrite_existing, ec);
            if (!ec && dll) { ++pluginsCopied; }
        }
        std::printf("  plugins: %d DLL(s) copied from %s\n", pluginsCopied, from.string().c_str());
        setEnv("CASCADE_PLUGIN_DIR_TEST", to.string());   // the host's folder for this run (core/plugin_host.cpp defaultPluginDir): a writable exe folder would win otherwise
    } else {
        // no plugins for this run, whatever the build tree's exe folder holds
        fs::create_directories(dir / "local" / "foxsdr" / "plugins", ec);
        setEnv("CASCADE_PLUGIN_DIR_TEST", (dir / "local" / "foxsdr" / "plugins").string());
    }
    // THE USER'S RADIOS (docs 6.51): SoapySDR's vendor modules are looked for under the USER'S folders (source/soapy_modules.cpp candidateVendorRoots:
    // %USERPROFILE%\radioconda\Library, %LOCALAPPDATA%\radioconda\Library, Program Files\PothosSDR, SOAPY_SDR_ROOT), and this run is about to
    // redirect USERPROFILE and LOCALAPPDATA into the scratch profile - after which a saved B200 "did not reopen (absent)", the finding that cost
    // the first five-minute run its radio. A script that says `radios=user` keeps the per-user root reachable through SOAPY_SDR_ROOT, read
    // BEFORE the redirect; a run that does not say so stays hermetic (no module of the user's is loaded).
    if (h.userRadios && std::getenv("SOAPY_SDR_ROOT") == nullptr) {
        for (const char* base : {"USERPROFILE", "LOCALAPPDATA"}) {
            const char* v = std::getenv(base);
            if (v == nullptr || *v == '\0') { continue; }
            const fs::path root = fs::path(v) / "radioconda" / "Library";
            if (fs::is_directory(root / "lib" / "SoapySDR", ec)) {
                setEnv("SOAPY_SDR_ROOT", root.string());
                std::printf("  radios=user: SOAPY_SDR_ROOT=%s\n", root.string().c_str());
                break;
            }
        }
    }
    fs::create_directories(dir / "home", ec);
    fs::create_directories(dir / "diag", ec);
    setEnv("APPDATA", (dir / "appdata").string());
    setEnv("LOCALAPPDATA", (dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (dir / "local").string());
    setEnv("XDG_STATE_HOME", (dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (dir / "diag").string());
    setEnv("USERPROFILE", (dir / "home").string());
    setEnv("HOME", (dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    const fs::path cfg = dir / "config.json";
    if (!h.config.empty()) {
        // THE SEED FILE (docs 6.51): a complete settings document of the author's - a real radio's profile, say - copied as the run's config.json.
        // It is the author's to keep hermetic (telemetryEnabled false, updateCheckEnabled false); the run still redirects every folder and endpoint.
        fs::path seed(h.config);
        if (seed.is_relative()) {
            const char* fx = std::getenv("FOXSDR_DRIVE_FIXTURES_DIR");
            seed = (fx != nullptr && *fx != '\0') ? fs::path(fx) / seed : fs::path(FOXSDR_DRIVE_FIXTURES_DIR) / seed;
        }
        const std::string text = readFile(seed);
        if (text.empty()) { std::printf("  the config seed %s is missing or empty\n", seed.string().c_str()); }
        CHECK(!text.empty());
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << text;
    } else {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": " << (h.updateCheck ? "true" : "false") << ", \"mainView\": \"receiver\", \"bandPlanOverlay\": false";
        if (h.iqfile.empty()) {
            f << ", \"sourceKind\": \"siggen\"";
        } else {
            // THE FIXTURE RUN (docs 6.50): the file source plays the WAV in a loop; its centre is the tune, and the demodulator and passband are the
            // plugin's. A relative path is under src/slint/tests/drive/fixtures (FOXSDR_DRIVE_FIXTURES_DIR names another folder).
            fs::path wav(h.iqfile);
            if (wav.is_relative()) {
                const char* fx = std::getenv("FOXSDR_DRIVE_FIXTURES_DIR");
                wav = (fx != nullptr && *fx != '\0') ? fs::path(fx) / wav : fs::path(FOXSDR_DRIVE_FIXTURES_DIR) / wav;
            }
            std::string p = wav.string();
            std::string esc;
            for (char c : p) { if (c == '\\') { esc += "\\\\"; } else if (c == '"') { esc += "\\\""; } else { esc += c; } }
            f << ", \"sourceKind\": \"file\", \"iqFilePath\": \"" << esc << "\"";
            if (!h.centre.empty()) { f << ", \"centerHz\": " << h.centre; }
            if (!h.mode.empty()) { f << ", \"mode\": \"" << h.mode << "\""; }
            if (!h.bandwidth.empty()) { f << ", \"bandwidthHz\": " << h.bandwidth; }
            // The WAVs are not committed (tens of megabytes each): a missing fixture is made by its generator, make_<stem>.py beside it,
            // with the standard-library Python the build found (deterministic, so every machine gets the same bytes).
            if (!fs::exists(wav)) {
                const fs::path gen = wav.parent_path() / ("make_" + wav.stem().string() + ".py");
                if (fs::exists(gen)) {
                    std::printf("  making the fixture %s with %s\n", p.c_str(), gen.filename().string().c_str());
                    const Run g = run("\"" FOXSDR_DRIVE_PYTHON "\" -I \"" + gen.string() + "\" 2>&1");
                    if (g.exitCode != 0) { std::printf("  the generator failed (%d):\n%s\n", g.exitCode, g.out.c_str()); }
                }
            }
            if (!fs::exists(wav)) { std::printf("  the fixture %s does not exist\n", p.c_str()); }
            CHECK(fs::exists(wav));
        }
        f << " }\n";
    }
    const fs::path scriptPath = dir / "input.txt";
    {
        std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
        f << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_INPUT_SCRIPT", scriptPath.string());
    // A bounded run keeps the receiver STOPPED unless CASCADE_DECODE_TEST asks (engine/window_service_options.cpp); a driven run wants the
    // engine running as a session has it - the generator fed, the scanner able to scan, STOP meaning something. A header line
    // "CASCADE_DECODE_TEST=" (empty) keeps the receiver stopped for a script that wants that.
    setEnv("CASCADE_DECODE_TEST", "1");
    for (const auto& [k, v] : h.env) { setEnv(k.c_str(), v); }
    // THE CANNED UPDATE ANSWER (docs 6.63): a script that says `manifest=` is given foxsdr.com's answer as a file, under the fixtures folder when relative; every other
    // run has none (the test process may have inherited one), so the check is never asked and never answered.
    {
        fs::path m(h.manifest);
        if (!h.manifest.empty() && m.is_relative()) {
            const char* fx = std::getenv("FOXSDR_DRIVE_FIXTURES_DIR");
            m = (fx != nullptr && *fx != '\0') ? fs::path(fx) / m : fs::path(FOXSDR_DRIVE_FIXTURES_DIR) / m;
        }
        if (!h.manifest.empty() && !fs::exists(m)) { std::printf("  the manifest %s does not exist\n", m.string().c_str()); }
        CHECK(h.manifest.empty() || fs::exists(m));
        setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", h.manifest.empty() ? std::string() : m.string());
    }

    // The window under test; FOXSDR_DRIVE_EXE names another build of it for this run (docs 6.62: the clip sweep runs the probe build, whose Text elements report an overflow).
    std::string exe = FOXSDR_SLINT_EXE;
    if (const char* other = std::getenv("FOXSDR_DRIVE_EXE"); other != nullptr && *other != '\0') { exe = other; }
    const Run r = run("\"" + exe + "\" --frames " + std::to_string(h.frames) + " 2>&1");
    const std::string ticks = std::to_string(h.frames) + " of " + std::to_string(h.frames) + " ticks, exit 0";
    const bool finished = r.out.find(ticks) != std::string::npos;
    std::string summary;
    {
        const std::size_t at = r.out.find("input script: ");
        if (at != std::string::npos) {
            const std::size_t end = r.out.find('\n', at);
            summary = r.out.substr(at, end == std::string::npos ? std::string::npos : end - at);
            // the summary is the LAST such line (the first is "input script: N steps from <file>")
            std::size_t more = r.out.find("input script: ", at + 1);
            while (more != std::string::npos) {
                const std::size_t e2 = r.out.find('\n', more);
                const std::string cand = r.out.substr(more, e2 == std::string::npos ? std::string::npos : e2 - more);
                if (cand.find(" steps, ") != std::string::npos) { summary = cand; }
                more = r.out.find("input script: ", more + 1);
            }
        }
    }
    const bool clean = summary.find(", 0 failed, 0 not reached") != std::string::npos;
    CHECK(r.exitCode == 0);
    CHECK(finished);
    CHECK(clean);
    // THE LOG (docs 6.63): the lines a script's header asks for (`logcontains=`) are looked for in every file the run's diagnostic folder holds.
    for (const std::string& want : h.logContains) {
        bool found = false;
        std::error_code lec;
        for (const auto& entry : fs::recursive_directory_iterator(dir / "diag", fs::directory_options::skip_permission_denied, lec)) {
            if (!entry.is_regular_file(lec)) { continue; }
            if (readFile(entry.path()).find(want) != std::string::npos) { found = true; break; }
        }
        if (!found) { std::printf("  the run's log does not hold \"%s\"\n", want.c_str()); }
        CHECK(found);
    }
    // FOXSDR_DRIVE_VERBOSE=1 prints the whole run (a script author reads its `dump` from here); a failed run prints it anyway.
    const char* verbose = std::getenv("FOXSDR_DRIVE_VERBOSE");
    if (r.exitCode != 0 || !finished || !clean || (verbose != nullptr && *verbose == '1')) {
        std::printf("  %s: exit %d; %s\n--- the run's output ---\n%s\n--- end ---\n", tag.c_str(), r.exitCode, summary.c_str(), r.out.c_str());
    }
    if (r.exitCode == 0 && finished && clean) {
        std::printf("  %s: %s\n", tag.c_str(), summary.c_str());
        // the step lines, for the record
        std::istringstream lines(r.out);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find("input script line ") != std::string::npos) { std::printf("  %s\n", line.c_str()); }
        }
    }
    if (tag == "builtin") {
        CHECK(r.out.find("input script line 3: dump -> ok") != std::string::npos);
        CHECK(r.out.find("labelled controls") != std::string::npos);
        CHECK(fs::exists(dir / "screenshots" / "drive-run.png"));
    }
    return testSummary("test_slint_drive_run");
}
