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
    // P9 fields, both away from their defaults (and the URL empty-adjacent
    // junk, so a load path that forgets to assign it is caught).
    c.pluginCatalogueUrl = "garbage";
    c.pluginBrowserOpen = true;
    // P10: away from its default AND out of range, so a load path that forgets
    // to assign it is caught by the same rule as every other field.
    c.pluginLastUpdateCheck = -999;
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
    CHECK(a.pluginCatalogueUrl == b.pluginCatalogueUrl);
    CHECK(a.pluginBrowserOpen == b.pluginBrowserOpen);
    CHECK(a.pluginLastUpdateCheck == b.pluginLastUpdateCheck);
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
        // P9: the enterprise escape hatch. A URL that is neither the default
        // nor junkConfig()'s value, so the roundtrip proves the FILE is what
        // came back rather than either end's fallback.
        in.pluginCatalogueUrl = "https://plugins.example.invalid/team/index.json";
        in.pluginBrowserOpen = true;
        // P10: a timestamp past 2038, which is why the field is 64-bit.
        in.pluginLastUpdateCheck = 4102444800;  // 2100-01-01T00:00:00Z

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
