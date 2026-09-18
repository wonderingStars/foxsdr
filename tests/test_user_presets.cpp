// Tests for the user's own presets (0.99.4): core/user_presets.hpp (identity,
// sanitising, add/remove), their round trip through the config file, and the
// GUI helpers in gui/tune_control.hpp that put them in front of the plugin's
// own presets when the host decides what to auto-apply.
//
// THE CASE THIS EXISTS FOR, pinned at the bottom: a UK listener saves
// 153.0500 MHz against POCSAG, whose own PRESET[0] is DAPNET on 439.9875 MHz.
// Opening POCSAG must then tune to 153.0500, and opening it while already on
// 153.0500 must tune nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/config.hpp"
#include "core/plugin_abi.h"
#include "core/user_presets.hpp"
#include "gui/tune_control.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;
using cascade::core::UserPreset;
using cascade::core::UserPresetAdd;

namespace {

UserPreset up(const std::string& plugin, double hz, const std::string& label = "",
              std::uint32_t mode = CASCADE_DEMOD_NFM, double bw = 12500.0) {
    UserPreset p;
    p.plugin = plugin;
    p.label = label;
    p.frequencyHz = hz;
    p.demodMode = mode;
    p.bandwidthHz = bw;
    return p;
}

CascadePreset pluginPreset(double hz, std::uint32_t flags = 0u) {
    CascadePreset ps{};
    ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
    std::snprintf(ps.label, sizeof(ps.label), "%s", "plugin");
    ps.frequencyHz = hz;
    ps.demodMode = CASCADE_DEMOD_NFM;
    ps.bandwidthHz = 12500.0;
    ps.flags = flags;
    return ps;
}

}  // namespace

int main() {
    using cascade::core::userPresetKey;

    // --- IDENTITY: the version-stripped module id ---------------------------
    // The whole reason it is not the file name: an update must not orphan the
    // presets saved against the previous version.
    CHECK(userPresetKey("pocsag-decoder-1.0.2-abi3-win-x64.dll") == "pocsag-decoder");
    CHECK(userPresetKey("pocsag-decoder-1.0.3-abi3-win-x64.dll") == "pocsag-decoder");
    CHECK(userPresetKey("pocsag-decoder-1.0.2-abi3-linux-x64.so") == "pocsag-decoder");
    CHECK(userPresetKey("C:\\Users\\x\\plugins\\flex-decoder-1.0.0-abi3-win-x64.dll") ==
          "flex-decoder");
    CHECK(userPresetKey("/opt/p/adsb-decoder-1.6.0-abi3-linux-x64.so") == "adsb-decoder");
    // No version in the name: the stem, unchanged.
    CHECK(userPresetKey("my-decoder.dll") == "my-decoder");
    CHECK(userPresetKey("plain") == "plain");
    // A digit that is not a full x.y.z version does not end the id.
    CHECK(userPresetKey("sat-2-tracker.dll") == "sat-2-tracker");
    CHECK(userPresetKey("x-1.2.dll") == "x-1.2");
    CHECK(userPresetKey("") == "");

    // --- FREQUENCY VALIDITY -------------------------------------------------
    CHECK(cascade::core::userPresetFrequencyValid(153.05e6));
    CHECK(!cascade::core::userPresetFrequencyValid(0.0));
    CHECK(!cascade::core::userPresetFrequencyValid(-1.0));
    CHECK(!cascade::core::userPresetFrequencyValid(std::numeric_limits<double>::quiet_NaN()));
    CHECK(!cascade::core::userPresetFrequencyValid(std::numeric_limits<double>::infinity()));
    CHECK(!cascade::core::userPresetFrequencyValid(200.0e9));

    CHECK(cascade::core::defaultUserPresetLabel(153.05e6) == "153.0500 MHz");

    // --- SANITISING: element-wise, first wins, caps -------------------------
    {
        std::vector<UserPreset> in;
        in.push_back(up("pocsag-decoder", 153.05e6, "PageOne"));
        in.push_back(up("", 153.1e6));                                // no key
        in.push_back(up("pocsag-decoder", std::nan("")));             // NaN
        in.push_back(up("pocsag-decoder", 153.05e6 + 50.0, "dup"));   // same channel
        in.push_back(up("flex-decoder", 153.05e6, "same hz, other plugin"));
        in.push_back(up("pocsag-decoder", 153.35e6, "", 99u, -5.0));  // bad mode/bw, no label
        in.push_back(up(std::string(200, 'k'), 153.2e6));             // over-long key
        const std::vector<UserPreset> out = cascade::core::sanitiseUserPresets(in);
        CHECK(out.size() == 3u);
        if (out.size() == 3u) {
            CHECK(out[0].label == "PageOne");
            CHECK(out[1].plugin == "flex-decoder");
            CHECK(out[2].frequencyHz == 153.35e6);
            CHECK(out[2].demodMode == 0u);
            CHECK(out[2].bandwidthHz == 0.0);
            CHECK(out[2].label == "153.3500 MHz");
        }
    }
    {
        // Control characters stripped; a UTF-8 character straddling the byte
        // budget is dropped whole, never split.
        std::string label = "Line\none\t";
        std::vector<UserPreset> in{up("a", 1.0e6, label)};
        std::vector<UserPreset> out = cascade::core::sanitiseUserPresets(in);
        CHECK(out.size() == 1u && out[0].label == "Lineone");

        std::string longLabel(46, 'x');
        longLabel += "\xC3\xA9";  // e-acute: bytes 46 and 47, the second one past the budget
        in = {up("a", 1.0e6, longLabel)};
        out = cascade::core::sanitiseUserPresets(in);
        CHECK(out.size() == 1u && out[0].label == std::string(46, 'x'));
        CHECK(out.size() == 1u && out[0].label.size() <= cascade::core::kMaxUserPresetLabelBytes);
    }
    {
        // Per-plugin cap.
        std::vector<UserPreset> in;
        for (int i = 0; i < 12; ++i) { in.push_back(up("p", 100.0e6 + i * 1.0e6)); }
        const std::vector<UserPreset> out = cascade::core::sanitiseUserPresets(in);
        CHECK(out.size() == cascade::core::kMaxUserPresetsPerPlugin);
    }
    {
        // Overall cap.
        std::vector<UserPreset> in;
        for (int i = 0; i < 400; ++i) {
            in.push_back(up("p" + std::to_string(i), 100.0e6));
        }
        const std::vector<UserPreset> out = cascade::core::sanitiseUserPresets(in);
        CHECK(out.size() == cascade::core::kMaxUserPresets);
    }

    // --- ADD / REMOVE -------------------------------------------------------
    {
        std::vector<UserPreset> list;
        CHECK(cascade::core::addUserPreset(list, up("pocsag-decoder", 153.05e6)) ==
              UserPresetAdd::Added);
        CHECK(list.size() == 1u && list[0].label == "153.0500 MHz");
        CHECK(cascade::core::addUserPreset(list, up("pocsag-decoder", 153.05e6 + 10.0)) ==
              UserPresetAdd::AlreadySaved);
        CHECK(cascade::core::addUserPreset(list, up("pocsag-decoder", 0.0)) ==
              UserPresetAdd::Invalid);
        CHECK(cascade::core::addUserPreset(list, up("", 153.2e6)) == UserPresetAdd::Invalid);
        CHECK(list.size() == 1u);
        for (int i = 1; i < 8; ++i) {
            CHECK(cascade::core::addUserPreset(list, up("pocsag-decoder", 153.0e6 + i * 1.0e5)) ==
                  UserPresetAdd::Added);
        }
        CHECK(cascade::core::addUserPreset(list, up("pocsag-decoder", 160.0e6)) ==
              UserPresetAdd::PluginFull);
        CHECK(list.size() == 8u);
        // Another plugin is unaffected by POCSAG being full.
        CHECK(cascade::core::addUserPreset(list, up("flex-decoder", 160.0e6)) ==
              UserPresetAdd::Added);

        CHECK(cascade::core::userPresetsFor(list, "pocsag-decoder").size() == 8u);
        CHECK(cascade::core::userPresetsFor(list, "flex-decoder").size() == 1u);
        CHECK(cascade::core::userPresetsFor(list, "nobody").empty());

        // Remove by ordinal WITHIN the plugin, not by position in the list.
        const double secondHz = cascade::core::userPresetsFor(list, "pocsag-decoder")[1].frequencyHz;
        CHECK(cascade::core::removeUserPreset(list, "pocsag-decoder", 1u));
        CHECK(cascade::core::userPresetsFor(list, "pocsag-decoder").size() == 7u);
        for (const UserPreset& p : list) { CHECK(p.frequencyHz != secondHz || p.plugin != "pocsag-decoder"); }
        CHECK(!cascade::core::removeUserPreset(list, "pocsag-decoder", 7u));
        CHECK(!cascade::core::removeUserPreset(list, "nobody", 0u));
        CHECK(cascade::core::userPresetsFor(list, "flex-decoder").size() == 1u);
    }
    {
        // Overall cap on add.
        std::vector<UserPreset> list;
        for (std::size_t i = 0; i < cascade::core::kMaxUserPresets; ++i) {
            list.push_back(up("p" + std::to_string(i), 100.0e6));
        }
        CHECK(cascade::core::addUserPreset(list, up("new", 100.0e6)) == UserPresetAdd::ListFull);
    }

    // --- THE CONFIG FILE ROUND TRIP -----------------------------------------
    {
        const fs::path dir =
            fs::temp_directory_path() / ("user_presets_test_" + std::to_string(TEST_GETPID()));
        fs::create_directories(dir);
        const std::string path = (dir / "config.json").string();

        AppConfig cfg;
        cfg.userPresets.push_back(up("pocsag-decoder", 153.05e6, "PageOne 153.050"));
        cfg.userPresets.push_back(up("flex-decoder", 931.9375e6, "", CASCADE_DEMOD_NFM, 0.0));
        std::string err;
        CHECK(ConfigStore::save(path, cfg, err));
        AppConfig back;
        CHECK(ConfigStore::load(path, back, err));
        CHECK(back.userPresets.size() == 2u);
        if (back.userPresets.size() == 2u) {
            CHECK(back.userPresets[0].plugin == "pocsag-decoder");
            CHECK(back.userPresets[0].label == "PageOne 153.050");
            CHECK(back.userPresets[0].frequencyHz == 153.05e6);
            CHECK(back.userPresets[0].demodMode == CASCADE_DEMOD_NFM);
            CHECK(back.userPresets[0].bandwidthHz == 12500.0);
            // Saved with no label: the file carries the default, not "".
            CHECK(back.userPresets[1].label == "931.9375 MHz");
        }

        // A hand-edited file: one good entry among junk keeps the good one.
        {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            CHECK(f != nullptr);
            if (f != nullptr) {
                const char* text =
                    "{\"userPresets\": [42, \"x\", {\"plugin\": \"pocsag-decoder\", "
                    "\"frequencyHz\": 153050000, \"demodMode\": -3, \"label\": \"ok\"}, "
                    "{\"plugin\": \"pocsag-decoder\", \"frequencyHz\": \"nope\"}]}";
                std::fwrite(text, 1, std::strlen(text), f);
                std::fclose(f);
            }
            AppConfig junk;
            ConfigStore::load(path, junk, err);
            CHECK(junk.userPresets.size() == 1u);
            if (junk.userPresets.size() == 1u) {
                CHECK(junk.userPresets[0].label == "ok");
                CHECK(junk.userPresets[0].demodMode == 0u);
            }
        }
        // Not an array at all: no presets, and no crash.
        {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (f != nullptr) {
                const char* text = "{\"userPresets\": {\"a\": 1}}";
                std::fwrite(text, 1, std::strlen(text), f);
                std::fclose(f);
            }
            AppConfig junk;
            ConfigStore::load(path, junk, err);
            CHECK(junk.userPresets.empty());
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- GUI HELPERS ----------------------------------------------------------
    {
        using cascade::gui::abiDemodForModeIndex;
        // The exact inverse of applyPluginPreset's ABI-to-mode-button table.
        CHECK(abiDemodForModeIndex(0) == CASCADE_DEMOD_NFM);
        CHECK(abiDemodForModeIndex(1) == CASCADE_DEMOD_WFM);
        CHECK(abiDemodForModeIndex(2) == CASCADE_DEMOD_AM);
        CHECK(abiDemodForModeIndex(4) == CASCADE_DEMOD_USB);
        CHECK(abiDemodForModeIndex(6) == CASCADE_DEMOD_LSB);
        CHECK(abiDemodForModeIndex(7) == CASCADE_DEMOD_RAW);
        CHECK(abiDemodForModeIndex(-1) == CASCADE_DEMOD_UNCHANGED);
        CHECK(abiDemodForModeIndex(8) == CASCADE_DEMOD_UNCHANGED);

        const CascadePreset ps =
            cascade::gui::userPresetToCascade(up("pocsag-decoder", 153.05e6, "PageOne"));
        CHECK(ps.structSize == sizeof(CascadePreset));
        CHECK(std::strcmp(ps.label, "PageOne") == 0);
        CHECK(ps.frequencyHz == 153.05e6);
        CHECK(ps.demodMode == CASCADE_DEMOD_NFM);
        CHECK(ps.bandwidthHz == 12500.0);
        // A channel, not a device rate, and an absolute frequency.
        CHECK(ps.sampleRateHz == 0.0);
        CHECK(ps.flags == 0u);
        CHECK(cascade::gui::presetIsValid(ps));

        // The index space: a user preset's key can never be read as a plugin
        // preset's raw index (those are capped at 16).
        CHECK(!cascade::gui::isUserPresetIndex(0u));
        CHECK(!cascade::gui::isUserPresetIndex(15u));
        CHECK(cascade::gui::isUserPresetIndex(cascade::gui::kUserPresetIndexBase));
        const cascade::core::MutePreset mp =
            cascade::gui::userPresetToMutePreset(up("pocsag-decoder", 153.05e6, "PageOne"), 2u);
        CHECK(mp.index == cascade::gui::kUserPresetIndexBase + 2u);
        CHECK(mp.label == "PageOne");
        CHECK(!mp.deviceCentre);
    }

    // --- THE FIELD REPORT: POCSAG in the UK -----------------------------------
    {
        // POCSAG's own table as published in 1.0.2: DAPNET first, then the
        // four US 931 MHz channels.
        const std::vector<CascadePreset> pocsag{pluginPreset(439.9875e6),
                                                pluginPreset(931.9375e6),
                                                pluginPreset(931.9125e6),
                                                pluginPreset(931.8875e6),
                                                pluginPreset(931.4375e6)};
        const double rate = 2.4e6;

        // NO user preset: tuned to 153.050 by hand, opening POCSAG applies
        // DAPNET. This is the behaviour the tester reported.
        {
            const auto c = cascade::gui::autoPresetCandidates({}, pocsag);
            const int idx = cascade::gui::autoPresetIndexOnStart(c, 153.05e6, 0.0, rate);
            CHECK(idx == 0);
            CHECK(idx == 0 && c[0].frequencyHz == 439.9875e6);
        }
        // WITH a saved 153.050 and the radio somewhere else: opening POCSAG
        // applies the user's preset, not DAPNET.
        const std::vector<UserPreset> saved{up("pocsag-decoder", 153.05e6, "PageOne")};
        {
            const auto c = cascade::gui::autoPresetCandidates(saved, pocsag);
            CHECK(c.size() == 6u);
            const int idx = cascade::gui::autoPresetIndexOnStart(c, 100.0e6, 0.0, rate);
            CHECK(idx == 0);
            CHECK(idx == 0 && c[0].frequencyHz == 153.05e6);
        }
        // Already on the saved channel - by VFO offset from a nearby centre,
        // the way a user usually gets there: nothing is applied.
        {
            const auto c = cascade::gui::autoPresetCandidates(saved, pocsag);
            CHECK(cascade::gui::autoPresetIndexOnStart(c, 152.8e6, 0.25e6, rate) == -1);
        }
        // Already on one of the plugin's OWN channels: still nothing - a user
        // preset must not pull the radio off DAPNET either.
        {
            const auto c = cascade::gui::autoPresetCandidates(saved, pocsag);
            CHECK(cascade::gui::autoPresetIndexOnStart(c, 439.9875e6, 0.0, rate) == -1);
        }
        // And a plugin with NO preset table at all still gets the user's.
        {
            const auto c = cascade::gui::autoPresetCandidates(saved, {});
            CHECK(cascade::gui::autoPresetIndexOnStart(c, 100.0e6, 0.0, rate) == 0);
        }
    }

    return testSummary("test_user_presets");
}
