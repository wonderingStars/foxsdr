// Tests for core/config.hpp / config.cpp (ConfigStore).
//
// Every fixture file is synthesized in-test under one pid-suffixed directory
// in the CWD — ctest runs each test from build-<slug>/tests, which is
// gitignored — and the whole directory is removed on success, left behind on
// failure for autopsy.
//
// Reference checking: roundtrip equality is asserted field by field against
// the exact values written, never against re-serialized output. Float/double
// exactness through JSON is legitimate here: nlohmann emits round-trippable
// shortest representations, and float -> double -> text -> double -> float
// is lossless for values that started as floats.
//
// The atomicity tests are Windows-specific by nature (POSIX rename happily
// replaces an open file): the target is held open WITHOUT FILE_SHARE_DELETE,
// which blocks the rename step while still permitting a plain write — so an
// implementation that "saves" by writing the target directly would succeed
// and clobber the file, turning both assertions red. A second variant locks
// with share mode 0 (fully exclusive) to match the contract wording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"

#include "core/plugin_repo.hpp"  // defaultIndexUrl(), the default catalogue URL
// mapGeometryOnScreen(): the SECOND half of validating a saved map window.
// ConfigStore decides whether the numbers are sane; this decides whether the
// rectangle they describe still exists on this machine, and the two only make
// sense read together — so the cases live in one file. Pulls in no GLFW or
// ImGui; see the note at the top of app_window.hpp.
#include "gui/app_window.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

using cascade::core::AppConfig;
using cascade::core::ConfigStore;
namespace fs = std::filesystem;

namespace {

std::string g_root;  // per-process fixture directory, set in main()

std::string p(const char* rel) { return g_root + "/" + rel; }

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// Deliberately out-of-range / nonsensical prefill: proves load() assigns
// every field even on the failure paths instead of leaving stale state.
AppConfig junkConfig() {
    AppConfig c;
    c.schemaVersion = -7;
    c.sourceKind = "garbage";
    c.soapyArgs = "garbage";
    c.iqFilePath = "garbage";
    c.centerHz = -1.0;
    c.mode = "garbage";
    c.bandwidthHz = -2.0;
    c.squelchDb = 999.0f;
    c.volume = 42.0f;
    c.dbMin = 5.0f;
    c.dbMax = -5.0f;
    c.splitRatio = 77.0f;
    c.vfoOffsetHz = 1e18;
    c.sampleRateHz = -3.0;
    // P7 fields, every one set AWAY from its default (and out of range where
    // the field has one) so a load path that forgets to assign it is caught.
    c.stereoEnabled = false;
    c.deemphasisIndex = 99;
    c.nrEnabled = true;
    c.nrStrength = -4.0f;
    c.notchEnabled = true;
    c.notchFreqHz = -1.0;
    c.notchQ = 1e9;
    c.autoNotch = true;
    c.bandPlanOverlay = false;
    // Map geometry, away from the "nothing saved" default and out of range, so
    // a load path that forgets to assign it is caught.
    c.mapWindowWidth = -5;
    c.mapWindowHeight = 999999;
    c.mapWindowX = -999999;
    c.mapWindowY = 999999;
    // A map page that must never survive a load: a config that does not
    // mention pages has none, and a leftover here would restore a window
    // rectangle nobody saved.
    c.mapPages = {{"junk-page", -5, 999999, -5, 999999, true}};
    // A receiver position that must never survive a load: a config that does
    // not mention one has none, and a leftover here would put every distance
    // and bearing on the map - and the whole coverage overlay - at a place the
    // user never set.
    c.rxPositionSet = true;
    c.rxLatDeg = 999.0;
    c.rxLonDeg = -999.0;
    // P9 fields, both away from their defaults (and the URL empty-adjacent
    // junk, so a load path that forgets to assign it is caught).
    c.pluginCatalogueUrl = "garbage";
    c.pluginBrowserOpen = true;
    // P10: away from its default AND out of range, so a load path that forgets
    // to assign it is caught by the same rule as every other field.
    c.pluginLastUpdateCheck = -999;
    // A grant that must never survive a load: a config that does not mention
    // tune permission has granted none, and a leftover here would mean a
    // plugin could move the receiver on the strength of stale memory.
    c.pluginTuneAllowed = {"junk-grant"};
    // A stop that must never survive a load either: a config that does not
    // mention stopped plugins has stopped none, and a leftover here would
    // silence a decoder the user never switched off.
    c.pluginsStopped = {"junk-stop.dll"};
    // And an override that must not survive either: a config that says nothing
    // about mute overrides has none, so every plugin's mute follows the rule
    // its capabilities imply. A leftover here would silence a decoder whose
    // author never asked for it, or un-silence one whose user did.
    c.pluginMuteOverride = {"junk-mute.dll"};
    // P11 web server: every field away from its default, and the two that
    // matter set to the DANGEROUS value — web access on, bound to every
    // interface — so any load path that forgets to assign them is caught by a
    // test rather than by a receiver that quietly answered the network.
    c.webEnabled = true;
    c.webBindAddress = "0.0.0.0";
    c.webPort = 1;
    c.webUsername = "garbage";
    c.webPasswordRecord = "garbage";
    return c;
}

// Field-by-field equality with one CHECK each, so a mismatch names the field.
void checkEqual(const AppConfig& a, const AppConfig& b) {
    CHECK(a.schemaVersion == b.schemaVersion);
    CHECK(a.sourceKind == b.sourceKind);
    CHECK(a.soapyArgs == b.soapyArgs);
    CHECK(a.iqFilePath == b.iqFilePath);
    CHECK(a.centerHz == b.centerHz);
    CHECK(a.mode == b.mode);
    CHECK(a.bandwidthHz == b.bandwidthHz);
    CHECK(a.squelchDb == b.squelchDb);
    CHECK(a.volume == b.volume);
    CHECK(a.dbMin == b.dbMin);
    CHECK(a.dbMax == b.dbMax);
    CHECK(a.splitRatio == b.splitRatio);
    CHECK(a.vfoOffsetHz == b.vfoOffsetHz);
    CHECK(a.sampleRateHz == b.sampleRateHz);
    CHECK(a.stereoEnabled == b.stereoEnabled);
    CHECK(a.deemphasisIndex == b.deemphasisIndex);
    CHECK(a.nrEnabled == b.nrEnabled);
    CHECK(a.nrStrength == b.nrStrength);
    CHECK(a.notchEnabled == b.notchEnabled);
    CHECK(a.notchFreqHz == b.notchFreqHz);
    CHECK(a.notchQ == b.notchQ);
    CHECK(a.autoNotch == b.autoNotch);
    CHECK(a.bandPlanOverlay == b.bandPlanOverlay);
    CHECK(a.mapWindowWidth == b.mapWindowWidth);
    CHECK(a.mapWindowHeight == b.mapWindowHeight);
    CHECK(a.mapWindowX == b.mapWindowX);
    CHECK(a.mapWindowY == b.mapWindowY);
    // One whole-container compare, the same way the three plugin-name lists
    // are checked below: indexing into a possibly-shorter vector inside a
    // record-and-continue harness is an out-of-bounds read in exactly the run
    // that has something to report.
    CHECK(a.mapPages == b.mapPages);
    CHECK(a.rxPositionSet == b.rxPositionSet);
    CHECK(a.rxLatDeg == b.rxLatDeg);
    CHECK(a.rxLonDeg == b.rxLonDeg);
    CHECK(a.pluginCatalogueUrl == b.pluginCatalogueUrl);
    CHECK(a.pluginBrowserOpen == b.pluginBrowserOpen);
    CHECK(a.pluginLastUpdateCheck == b.pluginLastUpdateCheck);
    CHECK(a.pluginTuneAllowed == b.pluginTuneAllowed);
    CHECK(a.pluginsStopped == b.pluginsStopped);
    CHECK(a.pluginMuteOverride == b.pluginMuteOverride);
    CHECK(a.webEnabled == b.webEnabled);
    CHECK(a.webBindAddress == b.webBindAddress);
    CHECK(a.webPort == b.webPort);
    CHECK(a.webUsername == b.webUsername);
    CHECK(a.webPasswordRecord == b.webPasswordRecord);
}

}  // namespace

int main() {
    g_root = "cfg_test_" + std::to_string(TEST_GETPID());
    fs::remove_all(g_root);  // stale debris from a failed prior run
    fs::create_directory(g_root);

    // --- defaultPath: documented shape, no filesystem side effects ----------
    {
        const std::string dp = ConfigStore::defaultPath();
        CHECK(!dp.empty());
        CHECK(dp.find("foxsdr") != std::string::npos);
        CHECK(dp.ends_with("config.json"));
        CHECK(!fs::exists(dp) || fs::is_regular_file(dp));  // never a dir
    }

    // --- missing file: defaults + true, error stays empty -------------------
    {
        AppConfig out = junkConfig();
        std::string err = "stale";
        CHECK(ConfigStore::load(p("never_written.json"), out, err));
        CHECK(err.empty());
        checkEqual(out, AppConfig{});
    }

    // --- roundtrip every field through a path needing new directories -------
    {
        AppConfig in;
        in.schemaVersion = 1;  // the only value that loads back (schema gate)
        in.sourceKind = "soapy";
        in.soapyArgs = "driver=uhd,serial=ABC123";
        in.iqFilePath = "C:/iq/capture_2msps.wav";
        in.centerHz = 433920000.0;
        in.mode = "USB";
        in.bandwidthHz = 2700.0;
        in.squelchDb = -63.5f;
        in.volume = 0.85f;
        in.dbMin = -97.0f;
        in.dbMax = -12.5f;  // -97 < -22.5: survives the span rule
        in.splitRatio = 0.62f;
        in.vfoOffsetHz = -125000.0;
        in.sampleRateHz = 2400000.0;
        // P7 fields, each set to something distinguishable from its default
        // AND from junkConfig()'s value, so the roundtrip proves the file is
        // what came back rather than either end's fallback.
        in.stereoEnabled = false;
        in.deemphasisIndex = 1;   // 75 us
        in.nrEnabled = true;
        in.nrStrength = 0.375f;   // dyadic: exact through float<->double<->text
        in.notchEnabled = true;
        in.notchFreqHz = 1234.5;
        in.notchQ = 42.25;
        in.autoNotch = true;
        in.bandPlanOverlay = false;
        // Map pages: two, in an order the roundtrip must preserve, each with a
        // rectangle nobody would arrive at by accident and one with a NEGATIVE
        // x, because a second monitor to the left of the primary one is the
        // ordinary case that a naive "must be positive" rule would silently
        // throw away. Opposite open flags, so a save that hard-coded either
        // value is caught. The LEGACY mapWindow* fields stay at their default
        // here on purpose: save() no longer writes those keys — the dedicated
        // map-window block below proves that — so non-default values could
        // never roundtrip and do not belong in a roundtrip fixture.
        in.mapPages = {{"ADS-B", -1600, 42, 1234, 987, true},
                       {"Satellite Tracker", 64, 128, 800, 600, false}};
        // The receiver's position: a real place with a NEGATIVE longitude and
        // fractional minutes, so a load path that rounded it, dropped the sign
        // or forgot the field entirely is caught. This is the field the
        // distance and bearing columns are measured from, so losing it silently
        // is the failure the whole persistence exists to prevent.
        in.rxPositionSet = true;
        in.rxLatDeg = 53.480759;
        in.rxLonDeg = -2.242631;
        // P9: the enterprise escape hatch. A URL that is neither the default
        // nor junkConfig()'s value, so the roundtrip proves the FILE is what
        // came back rather than either end's fallback.
        in.pluginCatalogueUrl = "https://plugins.example.invalid/team/index.json";
        in.pluginBrowserOpen = true;
        // P10: a timestamp past 2038, which is why the field is 64-bit.
        in.pluginLastUpdateCheck = 4102444800;  // 2100-01-01T00:00:00Z
        // Tune grants, in an order the roundtrip must preserve: the list is
        // what the permission check reads, and a name silently dropped by the
        // save is a permission the user gave and did not get back.
        in.pluginTuneAllowed = {"Satellite Tracker", "ADS-B"};
        // Stopped plugins, likewise in an order the roundtrip must preserve,
        // and deliberately different names from the grants above so a save
        // that crossed the two lists would show up here.
        in.pluginsStopped = {"sstv-decoder.dll", "ais-decoder.dll"};
        // And the mute overrides, again with names of their own, so the full
        // round trip proves the three lists stay three lists.
        in.pluginMuteOverride = {"pocsag-decoder.dll"};
        // P11: values distinguishable from both the defaults and junkConfig().
        in.webEnabled = true;
        in.webBindAddress = "192.168.1.20";
        in.webPort = 9090;
        in.webUsername = "steve";
        in.webPasswordRecord = "pbkdf2-sha256$10000$AAECAwQFBgcICQoLDA0ODw==$"
                               "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

        const std::string path = p("nested/deeper/config.json");
        std::string err = "stale";
        CHECK(ConfigStore::save(path, in, err));
        CHECK(err.empty());
        CHECK(fs::is_regular_file(path));  // dirs were created on demand

        AppConfig out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        CHECK(err.empty());
        checkEqual(out, in);
    }

    // --- empty object: every field keeps its default, returns true ----------
    {
        const std::string path = p("empty_object.json");
        CHECK(writeText(path, "{}\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        checkEqual(out, AppConfig{});
    }

    // --- unknown keys ignored; known keys still load; ints accepted as reals
    {
        const std::string path = p("unknown_keys.json");
        CHECK(writeText(path,
                        "{\"schemaVersion\":1,\"futureFeature\":{\"a\":[1,2]},"
                        "\"colorTheme\":\"neon\",\"centerHz\":144000000,"
                        "\"mode\":\"NFM\"}\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.centerHz == 144000000.0);  // JSON integer -> double field
        CHECK(out.mode == "NFM");
        CHECK(out.volume == 0.5f);  // untouched default
    }

    // --- wrong-typed fields: each keeps its default, the rest load, true ----
    {
        const std::string path = p("wrong_types.json");
        CHECK(writeText(path,
                        "{\"schemaVersion\":1,"
                        "\"volume\":\"loud\","       // string for float
                        "\"centerHz\":\"high\","     // string for double
                        "\"mode\":7,"                // number for string
                        "\"squelchDb\":true,"        // bool is NOT a number
                        "\"splitRatio\":[0.5],"      // array for float
                        "\"vfoOffsetHz\":null,"      // null for double
                        "\"bandwidthHz\":200000.0,"  // valid, must load
                        "\"sourceKind\":\"file\","   // valid, must load
                        // P7: booleans are strict — 1 and "true" are wrong
                        // types, not permissive spellings — and a real
                        // boolean must still load.
                        "\"stereoEnabled\":1,"
                        "\"nrEnabled\":\"true\","
                        "\"deemphasisIndex\":1.5,"   // non-integer number
                        "\"notchQ\":\"high\","
                        "\"autoNotch\":true}"        // valid, must load
                        "\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        CHECK(err.empty());
        const AppConfig d;
        CHECK(out.stereoEnabled == d.stereoEnabled);
        CHECK(out.nrEnabled == d.nrEnabled);
        CHECK(out.deemphasisIndex == d.deemphasisIndex);
        CHECK(out.notchQ == d.notchQ);
        CHECK(out.autoNotch);  // the one well-typed P7 field did load
        CHECK(out.volume == d.volume);
        CHECK(out.centerHz == d.centerHz);
        CHECK(out.mode == d.mode);
        CHECK(out.squelchDb == d.squelchDb);
        CHECK(out.splitRatio == d.splitRatio);
        CHECK(out.vfoOffsetHz == d.vfoOffsetHz);
        CHECK(out.bandwidthHz == 200000.0);
        CHECK(out.sourceKind == "file");
    }

    // --- corrupt JSON: defaults + false + error text -------------------------
    {
        const std::string path = p("corrupt.json");
        CHECK(writeText(path, "{ this is not json at all"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(!ConfigStore::load(path, out, err));
        CHECK(!err.empty());
        checkEqual(out, AppConfig{});
    }

    // --- valid JSON, non-object root: also corrupt --------------------------
    {
        const std::string path = p("array_root.json");
        CHECK(writeText(path, "[1,2,3]\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(!ConfigStore::load(path, out, err));
        CHECK(!err.empty());
        checkEqual(out, AppConfig{});

        const std::string path2 = p("number_root.json");
        CHECK(writeText(path2, "42\n"));
        AppConfig out2 = junkConfig();
        CHECK(!ConfigStore::load(path2, out2, err));
        checkEqual(out2, AppConfig{});
    }

    // --- schemaVersion 999: FULL defaults + false, later fields untrusted ---
    {
        const std::string path = p("schema_999.json");
        CHECK(writeText(path, "{\"schemaVersion\":999,\"volume\":0.9}\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(!ConfigStore::load(path, out, err));
        CHECK(!err.empty());
        checkEqual(out, AppConfig{});  // volume 0.9 must NOT have been taken

        // Non-integer schemaVersion is a mismatch too, not a "keep default".
        const std::string path2 = p("schema_string.json");
        CHECK(writeText(path2, "{\"schemaVersion\":\"one\"}\n"));
        AppConfig out2 = junkConfig();
        CHECK(!ConfigStore::load(path2, out2, err));
        checkEqual(out2, AppConfig{});
    }

    // --- clamps (documented in config.hpp) -----------------------------------
    {
        const std::string path = p("clamps.json");

        // volume high / splitRatio low
        CHECK(writeText(path, "{\"volume\":3.5,\"splitRatio\":0.01}\n"));
        AppConfig out;
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.volume == 1.0f);
        CHECK(out.splitRatio == 0.1f);

        // volume low / splitRatio high
        CHECK(writeText(path, "{\"volume\":-0.25,\"splitRatio\":1.5}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.volume == 0.0f);
        CHECK(out.splitRatio == 0.9f);

        // in-range values pass through unclamped
        CHECK(writeText(path, "{\"volume\":0.3,\"splitRatio\":0.55}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.volume == 0.3f);
        CHECK(out.splitRatio == 0.55f);

        // dbMin/dbMax span too small (5 dB): BOTH reset to defaults
        CHECK(writeText(path, "{\"dbMin\":-20.0,\"dbMax\":-15.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.dbMin == -110.0f);
        CHECK(out.dbMax == 0.0f);

        // inverted span: reset
        CHECK(writeText(path, "{\"dbMin\":0.0,\"dbMax\":-50.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.dbMin == -110.0f);
        CHECK(out.dbMax == 0.0f);

        // exactly 10 dB: contract is strict (dbMin < dbMax - 10), so reset
        CHECK(writeText(path, "{\"dbMin\":-80.0,\"dbMax\":-70.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.dbMin == -110.0f);
        CHECK(out.dbMax == 0.0f);

        // comfortably valid pair survives untouched
        CHECK(writeText(path, "{\"dbMin\":-95.0,\"dbMax\":-20.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.dbMin == -95.0f);
        CHECK(out.dbMax == -20.0f);

        // sourceKind outside the whitelist resets to the always-available one
        CHECK(writeText(path, "{\"sourceKind\":\"banana\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.sourceKind == "siggen");
    }

    // --- P7 clamps (documented in config.hpp) --------------------------------
    {
        const std::string path = p("clamps_p7.json");

        // deemphasisIndex indexes a THREE-entry table; an out-of-range value
        // would read past it, so both ends clamp rather than reset.
        CHECK(writeText(path, "{\"deemphasisIndex\":7}\n"));
        AppConfig out;
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.deemphasisIndex == 2);
        CHECK(writeText(path, "{\"deemphasisIndex\":-3}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.deemphasisIndex == 0);
        CHECK(writeText(path, "{\"deemphasisIndex\":1}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.deemphasisIndex == 1);  // in range: untouched

        // nrStrength is the module's [0, 1].
        CHECK(writeText(path, "{\"nrStrength\":9.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.nrStrength == 1.0f);
        CHECK(writeText(path, "{\"nrStrength\":-0.5}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.nrStrength == 0.0f);
        CHECK(writeText(path, "{\"nrStrength\":0.25}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.nrStrength == 0.25f);

        // notch frequency stays inside the audible span of the 48 kHz sink,
        // and Q inside the biquad's useful range.
        CHECK(writeText(path, "{\"notchFreqHz\":0.0,\"notchQ\":0.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.notchFreqHz == 10.0);
        CHECK(out.notchQ == 0.1);
        CHECK(writeText(path, "{\"notchFreqHz\":1e9,\"notchQ\":1e9}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.notchFreqHz == 20000.0);
        CHECK(out.notchQ == 1000.0);
        CHECK(writeText(path, "{\"notchFreqHz\":700.0,\"notchQ\":25.0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.notchFreqHz == 700.0);
        CHECK(out.notchQ == 25.0);

        // The audio-processing switches default OFF so an upgraded install
        // sounds exactly as it did; the two display/decode aids default ON.
        const AppConfig d;
        CHECK(!d.nrEnabled);
        CHECK(!d.notchEnabled);
        CHECK(!d.autoNotch);
        CHECK(d.deemphasisIndex == 0);
        CHECK(d.stereoEnabled);
        CHECK(d.bandPlanOverlay);
    }

    // --- map window geometry (documented in config.hpp) -----------------------
    {
        const std::string path = p("map_window.json");
        AppConfig out;
        std::string err;

        // Nothing saved is the default, and is what makes the window fall back
        // to a size derived from the monitor instead of a fixed constant.
        const AppConfig d;
        CHECK(d.mapWindowWidth == 0);
        CHECK(d.mapWindowHeight == 0);
        CHECK(d.mapWindowX == 0);
        CHECK(d.mapWindowY == 0);

        // A plausible rectangle survives untouched, negative coordinates
        // included: a monitor to the left of the primary one is ordinary.
        CHECK(writeText(path, "{\"mapWindowWidth\":1600,\"mapWindowHeight\":1000,"
                              "\"mapWindowX\":-1920,\"mapWindowY\":0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.mapWindowWidth == 1600);
        CHECK(out.mapWindowHeight == 1000);
        CHECK(out.mapWindowX == -1920);
        CHECK(out.mapWindowY == 0);

        // The extremes of the documented range are IN range, not out of it.
        CHECK(writeText(path, "{\"mapWindowWidth\":320,\"mapWindowHeight\":320,"
                              "\"mapWindowX\":-16384,\"mapWindowY\":16384}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.mapWindowWidth == 320);
        CHECK(out.mapWindowHeight == 320);
        CHECK(out.mapWindowX == -16384);
        CHECK(out.mapWindowY == 16384);

        // Absurd values are REJECTED AS A RECTANGLE: one bad component takes
        // all four with it, because half a rectangle is not a rectangle the
        // user chose. Each case below breaks exactly one component and asserts
        // the other three went too.
        struct Case { const char* json; const char* why; };
        const Case bad[] = {
            {"{\"mapWindowWidth\":0,\"mapWindowHeight\":900,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "width unset but height set"},
            {"{\"mapWindowWidth\":900,\"mapWindowHeight\":0,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "height unset but width set"},
            {"{\"mapWindowWidth\":319,\"mapWindowHeight\":900,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "width below the minimum"},
            {"{\"mapWindowWidth\":900,\"mapWindowHeight\":319,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "height below the minimum"},
            {"{\"mapWindowWidth\":-800,\"mapWindowHeight\":900,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "negative width"},
            {"{\"mapWindowWidth\":16385,\"mapWindowHeight\":900,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "width past the maximum"},
            {"{\"mapWindowWidth\":900,\"mapWindowHeight\":100000,\"mapWindowX\":10,"
             "\"mapWindowY\":10}", "height past the maximum"},
            {"{\"mapWindowWidth\":900,\"mapWindowHeight\":700,\"mapWindowX\":16385,"
             "\"mapWindowY\":10}", "x past the maximum"},
            {"{\"mapWindowWidth\":900,\"mapWindowHeight\":700,\"mapWindowX\":10,"
             "\"mapWindowY\":-16385}", "y past the minimum"},
        };
        for (const Case& c : bad) {
            CHECK(writeText(path, std::string(c.json) + "\n"));
            out = junkConfig();
            CHECK(ConfigStore::load(path, out, err));
            // Reported with the reason so a failure names the case.
            if (out.mapWindowWidth != 0 || out.mapWindowHeight != 0 ||
                out.mapWindowX != 0 || out.mapWindowY != 0) {
                std::printf("  (case: %s)\n", c.why);
            }
            CHECK(out.mapWindowWidth == 0);
            CHECK(out.mapWindowHeight == 0);
            CHECK(out.mapWindowX == 0);
            CHECK(out.mapWindowY == 0);
        }

        // These four keys are LEGACY, and their write rule was REFINED after
        // review (the earlier expectation here — "never written" — was
        // provably wrong): migration into a mapPages entry happens only when
        // a track-capable plugin creates a page, so a plugin-less launch that
        // saved without these keys destroyed the user's old map rectangle one
        // session before it could ever be inherited. The contract now: the
        // legacy keys are WRITTEN THROUGH while mapPages is still empty, and
        // the first real page entry retires them. Both halves are asserted,
        // because either alone can rot: keep-while-unspent is what the review
        // demanded, drop-once-spent is what stops the keys living for ever.
        AppConfig in;
        in.schemaVersion = 1;
        in.mapWindowWidth = 1570;
        in.mapWindowHeight = 1020;
        in.mapWindowX = 1226;
        in.mapWindowY = 169;

        // Unspent seed (no pages): the rectangle must survive a roundtrip.
        const std::string rt = p("map_window_roundtrip.json");
        CHECK(ConfigStore::save(rt, in, err));
        out = junkConfig();
        CHECK(ConfigStore::load(rt, out, err));
        CHECK(out.mapWindowWidth == 1570);
        CHECK(out.mapWindowHeight == 1020);
        CHECK(out.mapWindowX == 1226);
        CHECK(out.mapWindowY == 169);

        // Spent seed (a page exists): the legacy keys are retired. The
        // raw-text check is the half a load alone cannot prove — absent keys
        // and present-but-zero keys load identically.
        in.mapPages = {{"ADS-B", 1, 2, 300, 200, true}};
        const std::string rt2 = p("map_window_retired.json");
        CHECK(ConfigStore::save(rt2, in, err));
        CHECK(readAll(rt2).find("mapWindowWidth") == std::string::npos);
        CHECK(readAll(rt2).find("mapWindowHeight") == std::string::npos);
        CHECK(readAll(rt2).find("mapWindowX") == std::string::npos);
        CHECK(readAll(rt2).find("mapWindowY") == std::string::npos);
        out = junkConfig();
        CHECK(ConfigStore::load(rt2, out, err));
        CHECK(out.mapWindowWidth == 0);
        CHECK(out.mapWindowHeight == 0);
    }

    // --- per-plugin map pages (documented in config.hpp) ----------------------
    {
        const std::string path = p("map_pages.json");
        AppConfig out;
        std::string err;

        // No pages is the default: a first run has no windows to restore.
        CHECK(AppConfig{}.mapPages.empty());

        // A plausible list survives untouched — order, rectangles (negative x
        // included) and both values of the open flag.
        CHECK(writeText(
            path,
            "{\"mapPages\":[{\"plugin\":\"ADS-B\",\"x\":-1600,\"y\":42,"
            "\"width\":1234,\"height\":987,\"open\":true},"
            "{\"plugin\":\"Satellite Tracker\",\"x\":64,\"y\":128,"
            "\"width\":800,\"height\":600,\"open\":false}]}\n"));
        out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        const std::vector<AppConfig::MapPage> expectPlausible = {
            {"ADS-B", -1600, 42, 1234, 987, true},
            {"Satellite Tracker", 64, 128, 800, 600, false}};
        CHECK(out.mapPages == expectPlausible);

        // Hygiene, all in one file: a bad rectangle zeroes ONLY that entry's
        // rectangle — the entry itself, with its plugin name and open flag,
        // survives and falls back to default placement; an entry with an
        // empty plugin name is dropped (it can never match a plugin); a
        // duplicate keeps the first, so which entry restores a page's
        // geometry cannot depend on file order luck; and a non-object
        // element is skipped rather than taking the list with it.
        CHECK(writeText(
            path,
            "{\"mapPages\":[{\"plugin\":\"ADS-B\",\"x\":10,\"y\":10,"
            "\"width\":319,\"height\":600,\"open\":true},"
            "{\"plugin\":\"\",\"x\":10,\"y\":10,\"width\":900,\"height\":600},"
            "{\"plugin\":\"ADS-B\",\"x\":5,\"y\":5,\"width\":800,"
            "\"height\":500,\"open\":false},"
            "7,"
            "{\"plugin\":\"AIS\",\"x\":20,\"y\":30,\"width\":900,"
            "\"height\":700,\"open\":false}]}\n"));
        out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        const std::vector<AppConfig::MapPage> expectHygiene = {
            {"ADS-B", 0, 0, 0, 0, true}, {"AIS", 20, 30, 900, 700, false}};
        CHECK(out.mapPages == expectHygiene);

        // And the full roundtrip, which is the actual promise: a rectangle
        // the user dragged a page to must outlive the process.
        AppConfig in;
        in.schemaVersion = 1;
        in.mapPages = {{"POCSAG", 1226, 169, 1570, 1020, true}};
        const std::string rt = p("map_pages_roundtrip.json");
        CHECK(ConfigStore::save(rt, in, err));
        out = junkConfig();
        CHECK(ConfigStore::load(rt, out, err));
        CHECK(out.mapPages == in.mapPages);

        // THE CAP IS REAL, NOT DECORATIVE. kMaxMapPages bounds what a config
        // file can make the loader hold; until this block, no input in the
        // suite ever exceeded it, so deleting the cap was behaviourally
        // invisible to every test - the definition of a guard that is not
        // guarded. 80 entries in, exactly kMaxMapPages survive, in file
        // order, and the last kept one is the boundary entry.
        {
            std::string big = "{\"mapPages\":[";
            for (int pageNo = 0; pageNo < 80; ++pageNo) {
                if (pageNo != 0) { big += ","; }
                big += "{\"plugin\":\"P" + std::to_string(pageNo) +
                       "\",\"x\":1,\"y\":2,\"width\":300,\"height\":200,"
                       "\"open\":true}";
            }
            big += "]}\n";
            const std::string capPath = p("map_pages_cap.json");
            CHECK(writeText(capPath, big));
            out = junkConfig();
            CHECK(ConfigStore::load(capPath, out, err));
            CHECK(out.mapPages.size() == AppConfig::kMaxMapPages);
            CHECK(out.mapPages.front().plugin == "P0");
            CHECK(out.mapPages.back().plugin ==
                  "P" + std::to_string(AppConfig::kMaxMapPages - 1));
        }
    }

    // --- the receiver's own position (documented in config.hpp) ---------------
    {
        const std::string path = p("rx_position.json");
        AppConfig out;
        std::string err;

        // Unset is the default, and unset is what makes every distance and
        // bearing say "no RX" instead of measuring from the Gulf of Guinea.
        const AppConfig d;
        CHECK(d.rxPositionSet == false);
        CHECK(d.rxLatDeg == 0.0);
        CHECK(d.rxLonDeg == 0.0);

        // A real position survives untouched, negative longitude included.
        CHECK(writeText(path, "{\"rxPositionSet\":true,\"rxLatDeg\":53.480759,"
                              "\"rxLonDeg\":-2.242631}\n"));
        out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.rxPositionSet);
        CHECK(out.rxLatDeg == 53.480759);
        CHECK(out.rxLonDeg == -2.242631);

        // THE ORIGIN IS A PLACE. 0,0 with the flag set is a receiver on the
        // equator at the prime meridian, and must load as a SET position - it
        // is the case a bare "all zero means unset" sentinel would get wrong,
        // and the reason this field carries a flag of its own.
        CHECK(writeText(path,
                        "{\"rxPositionSet\":true,\"rxLatDeg\":0,\"rxLonDeg\":0}\n"));
        out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.rxPositionSet);
        CHECK(out.rxLatDeg == 0.0);
        CHECK(out.rxLonDeg == 0.0);

        // The extremes of the documented range are IN range: the poles and the
        // antimeridian are legal positions, not rounding slop.
        const char* edges[] = {
            "{\"rxPositionSet\":true,\"rxLatDeg\":90,\"rxLonDeg\":180}",
            "{\"rxPositionSet\":true,\"rxLatDeg\":-90,\"rxLonDeg\":-180}",
        };
        for (const char* e : edges) {
            CHECK(writeText(path, std::string(e) + "\n"));
            out = junkConfig();
            CHECK(ConfigStore::load(path, out, err));
            CHECK(out.rxPositionSet);
            CHECK(std::fabs(out.rxLatDeg) == 90.0);
            CHECK(std::fabs(out.rxLonDeg) == 180.0);
        }

        // OUT OF RANGE DISCARDS THE WHOLE POSITION, the same rule the map
        // rectangle follows: half a position clamped to a place nobody chose
        // would still be a place the coverage map measured from.
        struct RxCase { const char* json; const char* why; };
        const RxCase bad[] = {
            {"{\"rxPositionSet\":true,\"rxLatDeg\":90.001,\"rxLonDeg\":0}",
             "latitude past the north pole"},
            {"{\"rxPositionSet\":true,\"rxLatDeg\":-90.001,\"rxLonDeg\":0}",
             "latitude past the south pole"},
            {"{\"rxPositionSet\":true,\"rxLatDeg\":91,\"rxLonDeg\":0}",
             "latitude well out of range"},
            {"{\"rxPositionSet\":true,\"rxLatDeg\":0,\"rxLonDeg\":180.001}",
             "longitude past the antimeridian, east"},
            {"{\"rxPositionSet\":true,\"rxLatDeg\":0,\"rxLonDeg\":-180.001}",
             "longitude past the antimeridian, west"},
            {"{\"rxPositionSet\":true,\"rxLatDeg\":0,\"rxLonDeg\":360}",
             "longitude in 0..360 form, which this field is not"},
            // Coordinates without the flag are not a position either: nothing
            // set them, so nothing may read them.
            {"{\"rxPositionSet\":false,\"rxLatDeg\":53.48,\"rxLonDeg\":-2.24}",
             "coordinates present but the flag says unset"},
        };
        for (const RxCase& c : bad) {
            CHECK(writeText(path, std::string(c.json) + "\n"));
            out = junkConfig();
            CHECK(ConfigStore::load(path, out, err));
            if (out.rxPositionSet || out.rxLatDeg != 0.0 || out.rxLonDeg != 0.0) {
                std::printf("  (case: %s)\n", c.why);
            }
            CHECK(!out.rxPositionSet);
            CHECK(out.rxLatDeg == 0.0);
            CHECK(out.rxLonDeg == 0.0);
        }

        // A COORDINATE THAT OVERFLOWS A DOUBLE never reaches the range check at
        // all: JSON has no infinity, so nlohmann refuses the literal and the
        // whole file is corrupt. Asserted here rather than left as a case in
        // the table above, because "load fails and every field defaults" is a
        // different outcome from "this one position was discarded" — and it is
        // the outcome that makes an isfinite() test on these two fields
        // unreachable from a file, which is why the sanitizer states its range
        // POSITIVELY (a NaN fails `lat >= -90` without a separate test for it).
        const char* overflow[] = {
            "{\"rxPositionSet\":true,\"rxLatDeg\":51.5,\"rxLonDeg\":1e400}",
            "{\"rxPositionSet\":true,\"rxLatDeg\":-1e400,\"rxLonDeg\":0}",
        };
        for (const char* o : overflow) {
            CHECK(writeText(path, std::string(o) + "\n"));
            out = junkConfig();
            CHECK(!ConfigStore::load(path, out, err));
            CHECK(!err.empty());
            CHECK(!out.rxPositionSet);
            CHECK(out.rxLatDeg == 0.0);
            CHECK(out.rxLonDeg == 0.0);
        }

        // And a set position survives a full save/load roundtrip, which is the
        // actual promise: the antenna has not moved, so neither has the number.
        AppConfig in;
        in.schemaVersion = 1;
        in.rxPositionSet = true;
        in.rxLatDeg = -33.865143;
        in.rxLonDeg = 151.209900;
        const std::string rt = p("rx_position_roundtrip.json");
        CHECK(ConfigStore::save(rt, in, err));
        out = junkConfig();
        CHECK(ConfigStore::load(rt, out, err));
        CHECK(out.rxPositionSet);
        CHECK(out.rxLatDeg == -33.865143);
        CHECK(out.rxLonDeg == 151.209900);
    }

    // --- a saved rectangle that no monitor can still show ----------------------
    // The sanitizer above passes anything inside +/-16384, which says nothing
    // about whether the DISPLAY it was saved on still exists. Measured on the
    // real application before this check existed: a config holding
    // (-1500,300) 1301x999 restored to (-1282,300) - 19 px of window on
    // screen - and (-16000,-16000) restored to (-1282,-980), where the only
    // on-screen part is the 19x19 resize grip, the title bar is off the top,
    // and the map cannot be reached at all.
    {
        using cascade::gui::ScreenRect;
        using cascade::gui::mapGeometryOnScreen;

        // The machine this was reproduced on: one 5120x1440 display with a
        // 48 px taskbar.
        const std::vector<ScreenRect> one{{0.0f, 0.0f, 5120.0f, 1392.0f}};

        // The ordinary case: a window the user placed and left.
        CHECK(mapGeometryOnScreen(1174, 117, 1120, 930, one));
        CHECK(mapGeometryOnScreen(0, 0, 1120, 930, one));

        // The two measured failures, both refused.
        CHECK(!mapGeometryOnScreen(-1500, 300, 1301, 999, one));
        CHECK(!mapGeometryOnScreen(-16000, -16000, 1301, 999, one));

        // A TITLE BAR OFF THE TOP is unreachable even though most of the
        // window is on screen: an ImGui window is dragged by its title bar,
        // and the resize grip in the far corner can only resize.
        CHECK(!mapGeometryOnScreen(500, -40, 1120, 930, one));
        // One pixel of bar showing is not a bar.
        CHECK(!mapGeometryOnScreen(500, -29, 1120, 930, one));
        // The whole bar showing is.
        CHECK(mapGeometryOnScreen(500, 0, 1120, 930, one));

        // Horizontally: a sliver is refused, a grabbable width is kept. The
        // boundary is the documented 120 px.
        CHECK(!mapGeometryOnScreen(-1181, 100, 1200, 900, one));  // 19 px showing
        CHECK(!mapGeometryOnScreen(-1081, 100, 1200, 900, one));  // 119 px
        CHECK(mapGeometryOnScreen(-1080, 100, 1200, 900, one));   // 120 px
        // And off the right-hand end, which the ordinary "x >= 0" check misses.
        CHECK(!mapGeometryOnScreen(5101, 100, 1200, 900, one));   // 19 px
        CHECK(mapGeometryOnScreen(5000, 100, 1200, 900, one));    // 120 px

        // A SECOND MONITOR TO THE LEFT is the case a naive "must be positive"
        // rule would wrongly reject - and the same rectangle must be refused
        // once that monitor is gone. This pair is the whole point of taking
        // the monitor list as an argument.
        const std::vector<ScreenRect> two{{-1920.0f, 0.0f, 1920.0f, 1032.0f},
                                          {0.0f, 0.0f, 5120.0f, 1392.0f}};
        CHECK(mapGeometryOnScreen(-1500, 300, 1301, 999, two));
        CHECK(!mapGeometryOnScreen(-1500, 300, 1301, 999, one));

        // Degenerate inputs are refused rather than trusted: a zero or
        // negative size is not a window, and no monitors at all means there is
        // nothing to be reachable on.
        CHECK(!mapGeometryOnScreen(100, 100, 0, 900, one));
        CHECK(!mapGeometryOnScreen(100, 100, 1200, 0, one));
        CHECK(!mapGeometryOnScreen(100, 100, -1200, -900, one));
        CHECK(!mapGeometryOnScreen(100, 100, 1200, 900, {}));

        // A window SHORTER than a title bar cannot hide behind the strip
        // clamp: the config sanitizer's 320 px minimum makes this unreachable
        // in practice, and it is asserted so a future minimum cannot quietly
        // make a 10 px window "reachable".
        CHECK(!mapGeometryOnScreen(100, 100, 1200, 10, one));
    }

    // --- a saved rectangle that DOES NOT FIT WHERE IT SITS --------------------
    // Reachability is not the only thing a restored rectangle can get wrong. A
    // geometry saved on a taller or wider display comes back with its bottom -
    // and the resize grip with it - off the screen: draggable by the title bar,
    // impossible to shrink, and written back oversized on every frame.
    //
    // WHAT FITS, NOT A SHARE OF THE MONITOR. The first attempt reused
    // kMapWorkAreaShare (the DEFAULT size's 85%) as the limit, which shrank
    // windows that were entirely on screen and perfectly usable: measured on
    // the application, a saved 1120x1300 at 600,60 - bottom 1360 inside a 1392
    // px work area - was restored as 1120x1183 and the 1183 written back to the
    // config, so a chosen height could not survive a restart.
    {
        using cascade::gui::ScreenRect;
        using cascade::gui::mapClampRestoredSize;
        using cascade::gui::mapReachableMonitor;

        // The machine this was reproduced on, again: 5120x1440 with a 48 px
        // taskbar, so the work area ends at x 5120, y 1392.
        const std::vector<ScreenRect> one{{0.0f, 0.0f, 5120.0f, 1392.0f}};

        // A rectangle that already fits is left exactly alone.
        int w = 1120;
        int h = 930;
        mapClampRestoredSize(1174, 117, w, h, one);
        CHECK(w == 1120);
        CHECK(h == 930);

        // THE REGRESSION: a tall window that still ends above the taskbar is
        // the user's own choice and survives the restore untouched. 60 + 1300
        // = 1360 <= 1392, so nothing here is off the screen.
        w = 1120;
        h = 1300;
        mapClampRestoredSize(600, 60, w, h, one);
        CHECK(w == 1120);
        CHECK(h == 1300);

        // Saved on a 4K panel, reopened here: the bottom would fall past the
        // work area, so the height comes down to exactly what is left below
        // the window's own top (1392 - 117), and the width, which fits, does
        // not move.
        w = 1120;
        h = 1350;
        mapClampRestoredSize(1174, 117, w, h, one);
        CHECK(w == 1120);
        CHECK(h == 1275);

        // Too wide as well, from a wider desktop - and measured from where the
        // window sits, not from the origin: at x 400 there are 4720 px of work
        // area to the right of it, and at y 200 there are 1192 below.
        w = 5000;
        h = 1300;
        mapClampRestoredSize(400, 200, w, h, one);
        CHECK(w == 4720);
        CHECK(h == 1192);

        // The same size at the origin fits the screen whole, so it is NOT
        // touched - which is the case the 85% share used to shrink for no
        // reason at all.
        w = 5000;
        h = 1300;
        mapClampRestoredSize(0, 0, w, h, one);
        CHECK(w == 5000);
        CHECK(h == 1300);

        // A WINDOW PARKED WITH ONLY ITS TITLE BAR SHOWING is not shrunk to a
        // sliver. What fits below y 1362 is 30 px; restoring a 30 px window -
        // and saving it - would destroy the geometry outright, so the
        // sanitizer's own minimum is the floor and a little overhang is kept.
        w = 1120;
        h = 900;
        mapClampRestoredSize(600, 1362, w, h, one);
        CHECK(w == 1120);
        CHECK(h == AppConfig::kMapWindowMinPx);

        // THE MONITOR IT LANDED ON IS THE ONE THAT DECIDES, not the largest
        // one attached: a window whose title bar is on the small left-hand
        // display is measured against that display's edges even though a much
        // bigger one is plugged in beside it.
        const std::vector<ScreenRect> two{{-1920.0f, 0.0f, 1920.0f, 1032.0f},
                                          {0.0f, 0.0f, 5120.0f, 1392.0f}};
        CHECK(mapReachableMonitor(-1500, 300, 1301, 999, two) == 0);
        w = 1800;
        h = 999;
        mapClampRestoredSize(-1500, 300, w, h, two);
        CHECK(w == 1500);  // 0 - (-1500): the left monitor ends at x 0
        CHECK(h == 732);   // 1032 - 300
        // And the same rectangle on the big monitor is not touched.
        CHECK(mapReachableMonitor(1174, 117, 1800, 999, two) == 1);
        w = 1800;
        h = 999;
        mapClampRestoredSize(1174, 117, w, h, two);
        CHECK(w == 1800);
        CHECK(h == 999);

        // A rectangle that is not restorable at all is left untouched: the
        // caller falls back to the monitor-derived default rather than opening
        // a clamped window somewhere the user cannot reach.
        w = 1301;
        h = 999;
        mapClampRestoredSize(-16000, -16000, w, h, one);
        CHECK(w == 1301);
        CHECK(h == 999);
        CHECK(mapReachableMonitor(-16000, -16000, 1301, 999, one) == -1);

        // A work area the platform reports as empty is not reachable, so it
        // caps nothing either - which is what keeps the clamp from shrinking a
        // window to nothing on a monitor list that arrived as zeroes.
        const std::vector<ScreenRect> degenerate{{0.0f, 0.0f, 0.0f, 0.0f}};
        CHECK(mapReachableMonitor(0, 0, 1120, 930, degenerate) == -1);
        w = 1120;
        h = 930;
        mapClampRestoredSize(0, 0, w, h, degenerate);
        CHECK(w == 1120);
        CHECK(h == 930);
        // No monitors at all, same answer.
        w = 1120;
        h = 930;
        mapClampRestoredSize(0, 0, w, h, {});
        CHECK(w == 1120);
        CHECK(h == 930);
    }

    // --- the DEFAULT rectangle, position included -----------------------------
    // The default size is capped at 85% of the work area, but the default
    // POSITION was checked against nothing, so the map opened with its bottom -
    // and the resize grip in it - off the screen whenever the main window sat
    // low, and that rectangle was then persisted and restored verbatim (the
    // restore clamp above does not touch it: it is not too big, only in the
    // wrong place). Measured on the application with the main window dragged to
    // y 400: the map opened at (1248,491) 1120x1168, a bottom of 1659 against a
    // work area of 1392.
    {
        using cascade::gui::ScreenRect;
        using cascade::gui::mapPlaceDefaultRect;

        const std::vector<ScreenRect> one{{0.0f, 0.0f, 5120.0f, 1392.0f}};

        // THE REPRODUCTION, as numbers: the rectangle is moved up until it
        // ends exactly at the bottom of the work area, and keeps its size -
        // the height is what the "it needs to display without a scroller"
        // report asked for.
        float x = 1248.0f;
        float y = 491.0f;
        float w = 1120.0f;
        float h = 1168.0f;
        mapPlaceDefaultRect(x, y, w, h, one);
        CHECK_NEAR(w, 1120.0f, 0.5f);
        CHECK_NEAR(h, 1168.0f, 0.5f);
        CHECK_NEAR(x, 1248.0f, 0.5f);
        CHECK_NEAR(y, 224.0f, 0.5f);  // 1392 - 1168

        // A default that already fits is left exactly where it was asked for.
        x = 600.0f;
        y = 60.0f;
        w = 1120.0f;
        h = 1168.0f;
        mapPlaceDefaultRect(x, y, w, h, one);
        CHECK_NEAR(x, 600.0f, 0.5f);
        CHECK_NEAR(y, 60.0f, 0.5f);
        CHECK_NEAR(w, 1120.0f, 0.5f);
        CHECK_NEAR(h, 1168.0f, 0.5f);

        // Off the RIGHT-HAND end too, which is the same fault on the other
        // axis: the anchor sits 140 px inside the main window's right edge, and
        // a main window near the right of the desktop puts the map past it.
        x = 4500.0f;
        y = 100.0f;
        w = 1120.0f;
        h = 900.0f;
        mapPlaceDefaultRect(x, y, w, h, one);
        CHECK_NEAR(x, 4000.0f, 0.5f);  // 5120 - 1120
        CHECK_NEAR(y, 100.0f, 0.5f);

        // A SECOND MONITOR decides for a window that lands on it, exactly as
        // the restore clamp does: the left-hand panel is shorter, so the same
        // rectangle is pushed up further there.
        const std::vector<ScreenRect> two{{-1920.0f, 0.0f, 1920.0f, 1032.0f},
                                          {0.0f, 0.0f, 5120.0f, 1392.0f}};
        x = -1500.0f;
        y = 400.0f;
        w = 1120.0f;
        h = 900.0f;
        mapPlaceDefaultRect(x, y, w, h, two);
        CHECK_NEAR(x, -1500.0f, 0.5f);
        CHECK_NEAR(y, 132.0f, 0.5f);  // 1032 - 900

        // A rectangle BIGGER than the whole work area - only reachable through
        // the small-screen floor in mapDefaultSize - is shrunk to the work area
        // and put at its origin, rather than nudged off the top by the move.
        const std::vector<ScreenRect> tiny{{0.0f, 0.0f, 400.0f, 300.0f}};
        x = 300.0f;
        y = 300.0f;
        w = 480.0f;
        h = 360.0f;
        mapPlaceDefaultRect(x, y, w, h, tiny);
        CHECK_NEAR(w, 400.0f, 0.5f);
        CHECK_NEAR(h, 300.0f, 0.5f);
        CHECK_NEAR(x, 0.0f, 0.5f);
        CHECK_NEAR(y, 0.0f, 0.5f);

        // NO MONITOR LIST AT ALL - the headless --frames run - leaves the
        // proposal alone: inventing a screen would be worse than the anchor the
        // main viewport already agreed to.
        x = 1248.0f;
        y = 491.0f;
        w = 1120.0f;
        h = 1168.0f;
        mapPlaceDefaultRect(x, y, w, h, {});
        CHECK_NEAR(x, 1248.0f, 0.5f);
        CHECK_NEAR(y, 491.0f, 0.5f);
        CHECK_NEAR(w, 1120.0f, 0.5f);
        CHECK_NEAR(h, 1168.0f, 0.5f);
    }

    // --- P9 plugin browser settings -------------------------------------------
    {
        const std::string path = p("plugin_browser.json");

        // The default catalogue URL is PluginRepo's, not a copied literal.
        // Asserting the identity (rather than the string) is the point: two
        // copies of a security-relevant origin that can drift is the bug.
        const AppConfig d;
        CHECK(d.pluginCatalogueUrl == cascade::core::PluginRepo::defaultIndexUrl());
        CHECK(!d.pluginCatalogueUrl.empty());
        CHECK(!d.pluginBrowserOpen);  // the browser starts closed

        // An EMPTY url is a hand-edit, not "disable the catalogue": it must
        // come back as the default rather than leaving the browser pointed at
        // nothing it could ever fetch.
        CHECK(writeText(path, "{\"pluginCatalogueUrl\":\"\"}\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginCatalogueUrl == d.pluginCatalogueUrl);

        // A non-default https URL is kept verbatim — this is the whole point
        // of the field, and validating it a second time here would only add a
        // weaker copy of PluginRepo's rule.
        CHECK(writeText(path,
                        "{\"pluginCatalogueUrl\":\"https://mirror.example.invalid/i.json\","
                        "\"pluginBrowserOpen\":true}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginCatalogueUrl == "https://mirror.example.invalid/i.json");
        CHECK(out.pluginBrowserOpen);

        // A LOCAL path is also kept verbatim: an enterprise catalogue on a
        // share is a supported deployment, and the loader is not the place
        // that decides what a catalogue location may look like.
        CHECK(writeText(path,
                        "{\"pluginCatalogueUrl\":\"C:/deploy/foxsdr/index.json\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginCatalogueUrl == "C:/deploy/foxsdr/index.json");

        // Wrong types keep the defaults, like every other field.
        CHECK(writeText(path,
                        "{\"pluginCatalogueUrl\":42,\"pluginBrowserOpen\":\"yes\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginCatalogueUrl == d.pluginCatalogueUrl);
        CHECK(out.pluginBrowserOpen == d.pluginBrowserOpen);

        // And both survive a save/load roundtrip through a file the store
        // wrote itself (the path the app actually uses).
        AppConfig in;
        in.pluginCatalogueUrl = "\\\\corp-share\\sdr\\plugins\\index.json";
        in.pluginBrowserOpen = true;
        const std::string rt = p("plugin_browser_rt.json");
        CHECK(ConfigStore::save(rt, in, err));
        AppConfig back = junkConfig();
        CHECK(ConfigStore::load(rt, back, err));
        CHECK(back.pluginCatalogueUrl == in.pluginCatalogueUrl);
        CHECK(back.pluginBrowserOpen);
    }

    // --- P10 plugin version policy -------------------------------------------
    {
        const std::string path = p("plugin_policy.json");
        const AppConfig d;

        // "Never checked" is the default, and there is deliberately NO
        // auto-update switch in this struct: nothing in the product fetches a
        // catalogue on its own, so there is no setting that could turn a
        // launch into a network call. Retirement is enforced from the locally
        // cached policy instead (see PluginRepo), which needs no network and
        // therefore needs no consent to collect an IP address.
        CHECK(d.pluginLastUpdateCheck == 0);

        // An ordinary timestamp loads verbatim.
        CHECK(writeText(path, "{\"pluginLastUpdateCheck\":1755200000}\n"));
        AppConfig out = junkConfig();
        std::string err;
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginLastUpdateCheck == 1755200000);

        // Past 2038: the field is 64-bit precisely so this does not wrap.
        CHECK(writeText(path, "{\"pluginLastUpdateCheck\":4102444800}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginLastUpdateCheck == 4102444800);

        // Negative (hand-edit, or a clock that went backwards) resets to
        // "never" rather than reporting a time before the epoch to the UI.
        CHECK(writeText(path, "{\"pluginLastUpdateCheck\":-1}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginLastUpdateCheck == 0);

        // Wrong types keep the default, like every other field.
        CHECK(writeText(path, "{\"pluginLastUpdateCheck\":\"yesterday\"}\n"));
        out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginLastUpdateCheck == d.pluginLastUpdateCheck);
        CHECK(writeText(path, "{\"pluginLastUpdateCheck\":1.5}\n"));
        out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginLastUpdateCheck == d.pluginLastUpdateCheck);
    }

    // --- Plugin tune permission ----------------------------------------------
    {
        const std::string path = p("plugin_tune.json");
        const AppConfig d;

        // NO GRANTS BY DEFAULT. This is the security-relevant one: a fresh
        // install, a missing field and a wrong-typed field must all mean "no
        // plugin may move the receiver", because the check that enforces it
        // reads exactly this list.
        CHECK(d.pluginTuneAllowed.empty());

        AppConfig out = junkConfig();
        std::string err;
        CHECK(writeText(path, "{\"volume\":0.5}\n"));  // field absent entirely
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginTuneAllowed.empty());

        out = junkConfig();
        CHECK(writeText(path, "{\"pluginTuneAllowed\":\"Tracker\"}\n"));  // not an array
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginTuneAllowed.empty());

        // Compared as WHOLE VECTORS, never element by element after a size
        // check: CHECK records a failure and carries on, so an indexed read
        // guarded only by a preceding size CHECK is an out-of-bounds read the
        // moment the size is wrong - which is exactly the run where the test
        // needs to report rather than crash.
        using Names = std::vector<std::string>;

        // An ordinary list loads verbatim and in order.
        CHECK(writeText(path, "{\"pluginTuneAllowed\":[\"Tracker\",\"ADS-B\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginTuneAllowed == Names({"Tracker", "ADS-B"}));

        // Non-string elements are dropped and the usable ones kept: refusing
        // the whole file over one bad element would wipe every other setting.
        CHECK(writeText(path, "{\"pluginTuneAllowed\":[\"Tracker\",7,null,\"AIS\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginTuneAllowed == Names({"Tracker", "AIS"}));

        // Empty names can match no plugin, and a DUPLICATE would make revoking
        // look like it failed - the second copy would still be there.
        CHECK(writeText(path,
                        "{\"pluginTuneAllowed\":[\"Tracker\",\"\",\"Tracker\",\"AIS\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginTuneAllowed == Names({"Tracker", "AIS"}));

        // The cap: a file claiming thousands of grants is a hand-edit, and the
        // list is scanned on every request_tune.
        {
            std::string big = "{\"pluginTuneAllowed\":[";
            for (std::size_t i = 0; i < AppConfig::kMaxTuneGrants + 50u; ++i) {
                if (i != 0) { big += ","; }
                big += "\"p" + std::to_string(i) + "\"";
            }
            big += "]}\n";
            CHECK(writeText(path, big));
            CHECK(ConfigStore::load(path, out, err));
            CHECK(out.pluginTuneAllowed.size() == AppConfig::kMaxTuneGrants);
        }

        // And a roundtrip through a file the store wrote itself.
        AppConfig in;
        in.pluginTuneAllowed = {"Satellite Tracker"};
        const std::string rt = p("plugin_tune_rt.json");
        CHECK(ConfigStore::save(rt, in, err));
        AppConfig back = junkConfig();
        CHECK(ConfigStore::load(rt, back, err));
        CHECK(back.pluginTuneAllowed == Names({"Satellite Tracker"}));
    }

    // --- Plugins the user has stopped ----------------------------------------
    //
    // The same shape of list as the grants above, sanitised by the same code,
    // so it is asserted against the same cases: a stop that survives a launch
    // is the whole of what persisting it buys, and a stop the file cannot
    // express (empty) or states twice (duplicate) must not reach the runner.
    {
        const std::string path = p("plugin_stopped.json");
        const AppConfig d;
        using Names = std::vector<std::string>;

        // NOTHING IS STOPPED BY DEFAULT. A fresh install, a missing field and
        // a wrong-typed field all mean "every installed plugin runs" - the
        // behaviour every release before this one had.
        CHECK(d.pluginsStopped.empty());

        AppConfig out = junkConfig();
        std::string err;
        CHECK(writeText(path, "{\"volume\":0.5}\n"));  // field absent entirely
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginsStopped.empty());

        out = junkConfig();
        CHECK(writeText(path, "{\"pluginsStopped\":\"adsb.dll\"}\n"));  // not an array
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginsStopped.empty());

        // An ordinary list loads verbatim and in order.
        CHECK(writeText(path, "{\"pluginsStopped\":[\"adsb.dll\",\"ais.dll\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginsStopped == Names({"adsb.dll", "ais.dll"}));

        // Non-string elements are dropped, the usable ones kept.
        CHECK(writeText(path, "{\"pluginsStopped\":[\"adsb.dll\",7,null,\"ais.dll\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginsStopped == Names({"adsb.dll", "ais.dll"}));

        // Empty names match no module, and a DUPLICATE would make Start look
        // like it failed - the second copy would still be stopping it.
        CHECK(writeText(path,
                        "{\"pluginsStopped\":[\"adsb.dll\",\"\",\"adsb.dll\",\"ais.dll\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginsStopped == Names({"adsb.dll", "ais.dll"}));

        // The cap, for the same reason the grants have one.
        {
            std::string big = "{\"pluginsStopped\":[";
            for (std::size_t i = 0; i < AppConfig::kMaxTuneGrants + 50u; ++i) {
                if (i != 0) { big += ","; }
                big += "\"p" + std::to_string(i) + ".dll\"";
            }
            big += "]}\n";
            CHECK(writeText(path, big));
            CHECK(ConfigStore::load(path, out, err));
            CHECK(out.pluginsStopped.size() == AppConfig::kMaxTuneGrants);
        }

        // THE ROUND TRIP, which is the property the feature is sold on: a
        // plugin stopped in one session is still stopped in the next. The two
        // lists are written and read independently - a stop must not become a
        // tune grant, and revoking one must not start the other.
        AppConfig in;
        in.pluginsStopped = {"adsb-decoder-1.0.1-abi3-win-x64.dll"};
        in.pluginTuneAllowed = {"tracker.dll"};
        const std::string rt = p("plugin_stopped_rt.json");
        CHECK(ConfigStore::save(rt, in, err));
        AppConfig back = junkConfig();
        CHECK(ConfigStore::load(rt, back, err));
        CHECK(back.pluginsStopped == Names({"adsb-decoder-1.0.1-abi3-win-x64.dll"}));
        CHECK(back.pluginTuneAllowed == Names({"tracker.dll"}));
    }

    // --- Plugins whose mute setting is not the default -----------------------
    //
    // The THIRD list of module file names, sanitised by the same function, and
    // asserted here rather than assumed from the other two: the shared helper
    // is only shared for as long as somebody keeps calling it, and a list that
    // quietly stopped being sanitised would show up as a preference that
    // flipped twice and therefore did nothing.
    {
        const std::string path = p("plugin_mute.json");
        const AppConfig d;
        using Names = std::vector<std::string>;

        // NOBODY HAS OVERRIDDEN ANYTHING by default, which is what makes the
        // capability-derived default the live rule for a fresh install.
        CHECK(d.pluginMuteOverride.empty());

        AppConfig out = junkConfig();
        std::string err;
        CHECK(writeText(path, "{\"volume\":0.5}\n"));  // field absent entirely
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginMuteOverride.empty());

        out = junkConfig();
        CHECK(writeText(path, "{\"pluginMuteOverride\":\"adsb.dll\"}\n"));  // not an array
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginMuteOverride.empty());

        CHECK(writeText(path, "{\"pluginMuteOverride\":[\"adsb.dll\",\"sstv.dll\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginMuteOverride == Names({"adsb.dll", "sstv.dll"}));

        // Empties and duplicates: a duplicated override is a setting applied
        // twice, and "the opposite of the opposite of the default" is the
        // default - i.e. exactly the state the user did NOT choose.
        CHECK(writeText(
            path,
            "{\"pluginMuteOverride\":[\"adsb.dll\",\"\",\"adsb.dll\",7,\"sstv.dll\"]}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.pluginMuteOverride == Names({"adsb.dll", "sstv.dll"}));

        {
            std::string big = "{\"pluginMuteOverride\":[";
            for (std::size_t i = 0; i < AppConfig::kMaxTuneGrants + 50u; ++i) {
                if (i != 0) { big += ","; }
                big += "\"p" + std::to_string(i) + ".dll\"";
            }
            big += "]}\n";
            CHECK(writeText(path, big));
            CHECK(ConfigStore::load(path, out, err));
            CHECK(out.pluginMuteOverride.size() == AppConfig::kMaxTuneGrants);
        }

        // THE ROUND TRIP, with all three lists carrying different names at
        // once: an override must not become a stop or a grant, and none of the
        // three may be dropped by a save that only remembered two of them.
        AppConfig in;
        in.pluginMuteOverride = {"sstv-decoder-1.0.0-abi3-win-x64.dll"};
        in.pluginsStopped = {"ais-decoder-1.0.1-abi3-win-x64.dll"};
        in.pluginTuneAllowed = {"tracker.dll"};
        const std::string rt = p("plugin_mute_rt.json");
        CHECK(ConfigStore::save(rt, in, err));
        AppConfig back = junkConfig();
        CHECK(ConfigStore::load(rt, back, err));
        CHECK(back.pluginMuteOverride == Names({"sstv-decoder-1.0.0-abi3-win-x64.dll"}));
        CHECK(back.pluginsStopped == Names({"ais-decoder-1.0.1-abi3-win-x64.dll"}));
        CHECK(back.pluginTuneAllowed == Names({"tracker.dll"}));
    }

    // --- P11 web server settings ---------------------------------------------
    {
        const std::string path = p("web.json");
        const AppConfig d;

        // THE DEFAULTS ARE THE SAFE ONES, and this is the security-relevant
        // assertion in this file: a fresh install, a missing field and a
        // corrupt file must all mean "web access off, and if it is ever turned
        // on it listens only to this machine". Every other web assertion below
        // is about a hand-edit failing safe.
        CHECK(!d.webEnabled);
        CHECK(d.webBindAddress == "127.0.0.1");
        CHECK(d.webPort == 8073);
        CHECK(d.webPasswordRecord.empty());

        AppConfig out = junkConfig();
        std::string err;
        CHECK(writeText(path, "{\"volume\":0.5}\n"));  // web fields absent
        CHECK(ConfigStore::load(path, out, err));
        CHECK(!out.webEnabled);
        CHECK(out.webBindAddress == "127.0.0.1");
        CHECK(out.webPasswordRecord.empty());

        // AN EMPTIED ADDRESS MUST NOT WIDEN THE BINDING. web_policy reads ""
        // as "every interface", so a field a hand-edit blanked would mean the
        // opposite of the safe default. It comes back as loopback.
        CHECK(writeText(path, "{\"webEnabled\":true,\"webBindAddress\":\"\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webEnabled);
        CHECK(out.webBindAddress == "127.0.0.1");

        // The port is sanitized, because it drives a numeric control.
        CHECK(writeText(path, "{\"webPort\":80}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webPort == 8073);
        CHECK(writeText(path, "{\"webPort\":0}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webPort == 8073);
        CHECK(writeText(path, "{\"webPort\":70000}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webPort == 8073);
        CHECK(writeText(path, "{\"webPort\":9000}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webPort == 9000);  // in range: untouched

        // The ADDRESS is NOT validated here — evaluateBind is its one
        // enforcement point, and a second weaker copy in the loader is how the
        // two come to disagree. A nonsense value is kept verbatim and refused
        // later, where the refusal can explain itself.
        CHECK(writeText(path, "{\"webBindAddress\":\"nas.local\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webBindAddress == "nas.local");

        // Likewise the password record: a corrupt one is kept so the policy can
        // refuse the bind and say so, rather than being blanked here — which
        // would silently downgrade it to "no password set".
        CHECK(writeText(path, "{\"webPasswordRecord\":\"not-a-record\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webPasswordRecord == "not-a-record");

        // An emptied user name resets, like the catalogue URL.
        CHECK(writeText(path, "{\"webUsername\":\"\"}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webUsername == "admin");

        // Wrong types keep the defaults, like every other field. The one that
        // matters is webEnabled: a string "true" must NOT switch the server on.
        out = junkConfig();
        CHECK(writeText(path,
                        "{\"webEnabled\":\"true\",\"webPort\":\"8073\","
                        "\"webBindAddress\":42}\n"));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.webEnabled == d.webEnabled);
        CHECK(out.webPort == d.webPort);
        CHECK(out.webBindAddress == d.webBindAddress);

        // A corrupt file leaves the safe defaults, not junkConfig()'s
        // enabled-and-wide-open prefill.
        out = junkConfig();
        CHECK(writeText(path, "{ not json"));
        CHECK(!ConfigStore::load(path, out, err));
        CHECK(!out.webEnabled);
        CHECK(out.webBindAddress == "127.0.0.1");
    }

#ifdef _WIN32
    // --- ATOMICITY: locked target => save fails, original byte-intact -------
    {
        fs::create_directory(p("atomic"));
        const std::string path = p("atomic/config.json");
        AppConfig original;
        original.volume = 0.25f;
        std::string err;
        CHECK(ConfigStore::save(path, original, err));
        const std::string origBytes = readAll(path);
        CHECK(!origBytes.empty());

        // Variant 1 — the mutant killer. Share READ+WRITE but NOT DELETE:
        // rename-over is blocked, yet a naive direct rewrite of the target
        // would still open fine and clobber it. Atomic save must fail and
        // leave every original byte in place.
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        AppConfig update;
        update.volume = 0.75f;
        err.clear();
        CHECK(!ConfigStore::save(path, update, err));
        CHECK(!err.empty());
        CloseHandle(h);
        CHECK(readAll(path) == origBytes);

        // Variant 2 — fully exclusive open (share mode 0), per the contract
        // wording; both the temp write's rename and any direct write are
        // denied, and the original must again survive byte-for-byte.
        HANDLE h2 = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h2 != INVALID_HANDLE_VALUE);
        err.clear();
        CHECK(!ConfigStore::save(path, update, err));
        CHECK(!err.empty());
        CloseHandle(h2);
        CHECK(readAll(path) == origBytes);

        // No temp-file debris may survive a failed save.
        std::size_t entries = 0;
        for (const auto& e : fs::directory_iterator(p("atomic"))) {
            (void)e;
            ++entries;
        }
        CHECK(entries == 1u);

        // And the file still loads as the ORIGINAL config.
        AppConfig out = junkConfig();
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.volume == 0.25f);

        // Lock released: the same save now succeeds and the content flips.
        CHECK(ConfigStore::save(path, update, err));
        CHECK(ConfigStore::load(path, out, err));
        CHECK(out.volume == 0.75f);
    }
#endif

    // --- save into a path whose "directory" is a file: clean failure --------
    {
        const std::string blocker = p("blocker");
        CHECK(writeText(blocker, "not a directory\n"));
        AppConfig cfg;
        std::string err;
        CHECK(!ConfigStore::save(blocker + "/config.json", cfg, err));
        CHECK(!err.empty());
    }

    // --- load where the path is a directory: unreadable => defaults + false -
    {
        AppConfig out = junkConfig();
        std::string err;
        CHECK(!ConfigStore::load(g_root, out, err));
        CHECK(!err.empty());
        checkEqual(out, AppConfig{});
    }

    const int rc = testSummary("test_config");
    if (rc == 0) {
        std::error_code ec;
        fs::remove_all(g_root, ec);  // success: leave nothing behind
    }
    return rc;
}
