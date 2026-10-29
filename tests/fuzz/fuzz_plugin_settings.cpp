// Fuzz target: the settings strings a plugin sends the host - and the ones a hand-edited config.json holds for it (plugin containment P2,
// docs/engine-stage4.md 6.29).
//
// A plugin stores its own settings through settings_get / settings_set (core/plugin_api.hpp), and the host keeps them in config.json under the
// plugin's name. Both sides of that are third-party text: the plugin's key and value, and - on the next start - whatever is in the file. The
// host's rules are bounded keys ([A-Za-z0-9._-], 1..63), bounded values (under 4 KiB), UTF-8 only ("a settings value goes into a JSON config
// file, and JSON is UTF-8 by definition - refusing here is better than having the writer mangle it later"), at most 64 keys a plugin and 256 plugins.
//
// THE PROPERTIES, checked against an oracle written from RFC 3629 and not from the host's own check:
//   - every value the store holds is strictly valid UTF-8, has no NUL, and is shorter than the bound; every key is valid; the counts are within
//     their bounds, however the store was reached (the plugin's call, the host's own field, a loaded config);
//   - a store that is written as the config writes it (nlohmann::json, error_handler replace) and read back is the SAME store: the writer does
//     not mangle what the check let through;
//   - a get returns the value's whole length, copies no more than it was given room for, and terminates what it copies.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/plugin_api.hpp"
#include "fuzz_common.hpp"

using cascade::core::PluginApiClient;
using cascade::core::PluginApiCore;
using cascade::core::PluginSettingsMap;

namespace {

// Strict UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above U+10FFFF.
bool strictUtf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    auto cont = [&](std::size_t k) { return k < n && (static_cast<unsigned char>(s[k]) & 0xC0u) == 0x80u; };
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80u) { ++i; continue; }
        if (c >= 0xC2u && c <= 0xDFu) {
            if (!cont(i + 1)) { return false; }
            i += 2;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            if (!cont(i + 1) || !cont(i + 2)) { return false; }
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            if (c == 0xE0u && b1 < 0xA0u) { return false; }   // overlong
            if (c == 0xEDu && b1 > 0x9Fu) { return false; }   // a surrogate
            i += 3;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) { return false; }
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            if (c == 0xF0u && b1 < 0x90u) { return false; }   // overlong
            if (c == 0xF4u && b1 > 0x8Fu) { return false; }   // above U+10FFFF
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

void requireSane(const PluginSettingsMap& store) {
    FUZZ_REQUIRE(store.size() <= cascade::core::kMaxSettingsPlugins);
    for (const auto& [plugin, kv] : store) {
        FUZZ_REQUIRE(!plugin.empty());
        FUZZ_REQUIRE(kv.size() <= CASCADE_MAX_SETTINGS_PER_PLUGIN);
        for (const auto& [k, v] : kv) {
            FUZZ_REQUIRE(cascade::core::validSettingKey(k.c_str()));
            FUZZ_REQUIRE(k.size() == std::strlen(k.c_str()));
            FUZZ_REQUIRE(v.size() < CASCADE_SETTING_VALUE_BYTES);
            FUZZ_REQUIRE(v.find('\0') == std::string::npos);
            FUZZ_REQUIRE(strictUtf8(v));   // what the host calls valid is what the standard calls valid
        }
    }
}

// The store as config.json holds it, and back.
void requireSurvivesTheFile(const PluginSettingsMap& store) {
    // (A plugin NAME is the descriptor's own string, and nothing demands it be UTF-8; a name that is not could not come back from the file as the
    // key it went in as - the settings under it are the cost, noted in docs 6.29, and not what this property is about. Values and keys are.)
    PluginSettingsMap expected;
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [plugin, kv] : store) {
        if (!strictUtf8(plugin)) { continue; }
        expected[plugin] = kv;
        nlohmann::json one = nlohmann::json::object();
        for (const auto& [k, v] : kv) { one[k] = v; }
        j[plugin] = one;
    }
    const std::string text = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const nlohmann::json back = nlohmann::json::parse(text, nullptr, false);
    FUZZ_REQUIRE(!back.is_discarded());
    PluginSettingsMap again;
    for (auto it = back.begin(); it != back.end(); ++it) {
        for (auto kit = it.value().begin(); kit != it.value().end(); ++kit) { again[it.key()][kit.key()] = kit.value().get<std::string>(); }
    }
    FUZZ_REQUIRE(again == expected);   // the writer did not mangle a value the check let through
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    PluginApiCore core;
    const std::string key = "fuzz-1.0.0-abi3-linux-x64.so";
    PluginApiClient& c = core.client(key, "Fuzz Plugin");
    core.setLive(key, true);

    for (int step = 0; step < 96 && in.remaining() > 0; ++step) {
        switch (in.u8() % 6) {
            case 0: {   // the plugin stores a value
                const std::string k = in.str(in.inRange(0, 70));
                const std::string v = in.str(in.inRange(0, 400));
                const std::int32_t r = core.settingsSet(c, k.c_str(), v.c_str());
                FUZZ_REQUIRE(r == CASCADE_API_OK || r == CASCADE_API_BAD_ARGUMENT || r == CASCADE_API_OUT_OF_RANGE || r == CASCADE_API_LIMIT);
                break;
            }
            case 1: {   // ...reads one back, into a buffer of exactly the size it names
                const std::string k = in.str(in.inRange(0, 70));
                const std::size_t cap = in.inRange(0, 600);
                std::vector<char> buf(cap);
                const std::int32_t r = core.settingsGet(c, k.c_str(), cap != 0 ? buf.data() : nullptr, cap);
                if (r >= 0 && cap > 0) {
                    FUZZ_REQUIRE(std::memchr(buf.data(), '\0', cap) != nullptr);   // always terminated
                    FUZZ_REQUIRE(strictUtf8(std::string(buf.data())));              // and cut on a character
                }
                break;
            }
            case 2: {   // ...deletes one
                const std::string k = in.str(in.inRange(0, 70));
                (void)core.settingsSet(c, k.c_str(), nullptr);
                break;
            }
            case 3: {   // the host's own drawn field writes one, under the plugin's name
                const std::string k = in.str(in.inRange(0, 70));
                const std::string v = in.str(in.inRange(0, 400));
                (void)core.settingsUiSet("Fuzz Plugin", k.c_str(), v.c_str());
                break;
            }
            case 4: {   // ...and reads what it shows
                std::string out;
                (void)core.settingsUiGet("Fuzz Plugin", in.str(in.inRange(0, 70)).c_str(), out);
                break;
            }
            case 5: {   // a config.json that names plugins, keys and values of its own
                PluginSettingsMap m;
                const unsigned plugins = in.inRange(0, 4);
                for (unsigned p = 0; p < plugins; ++p) {
                    const std::string name = in.str(in.inRange(0, 40));
                    const unsigned keys = in.inRange(0, 6);
                    for (unsigned k = 0; k < keys; ++k) { m[name][in.str(in.inRange(0, 70))] = in.str(in.inRange(0, 300)); }
                }
                core.loadSettings(cascade::core::sanitisePluginSettings(m));
                break;
            }
        }
        requireSane(core.settingsSnapshot());
    }
    requireSurvivesTheFile(core.settingsSnapshot());
    return 0;
}
