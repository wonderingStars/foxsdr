// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// See config_halves.hpp: the table of which half of the settings file each AppConfig field belongs to, and the document a half crosses
// the table as.
#include "core/config_halves.hpp"

#include <set>

#include <nlohmann/json.hpp>

namespace cascade::core {

namespace {

using json = nlohmann::json;

// The keys of the interface's half: the names of its Interface and Shared rows. Built once.
const std::set<std::string>& interfaceKeys() {
    static const std::set<std::string> keys = [] {
        std::set<std::string> k;
        for (const ConfigFieldInfo& f : kConfigFields) {
            if (f.owner != ConfigOwner::Engine) { k.insert(f.name); }
        }
        return k;
    }();
    return keys;
}

}  // namespace

void copyEngineHalf(const AppConfig& from, AppConfig& to) {
    forEachConfigField([&](const char*, ConfigOwner owner, bool, auto p) {
        if (owner != ConfigOwner::Interface) { to.*p = from.*p; }
    });
}

void copyInterfaceHalf(const AppConfig& from, AppConfig& to, bool includeShared) {
    forEachConfigField([&](const char*, ConfigOwner owner, bool, auto p) {
        if (owner == ConfigOwner::Interface || (includeShared && owner == ConfigOwner::Shared)) { to.*p = from.*p; }
    });
}

bool interfaceHalvesEqual(const AppConfig& a, const AppConfig& b) {
    bool same = true;
    forEachConfigField([&](const char*, ConfigOwner owner, bool moves, auto p) {
        if (owner == ConfigOwner::Engine || moves) { return; }
        if (!(a.*p == b.*p)) { same = false; }
    });
    return same;
}

void clearBookkeeping(AppConfig& cfg) {
    static const AppConfig kDefaults;
    forEachConfigField([&](const char*, ConfigOwner, bool moves, auto p) {
        if (moves) { cfg.*p = kDefaults.*p; }
    });
}

std::string serializeInterfaceHalf(const AppConfig& cfg) {
    // Only the half is serialised: the rest of the document would be defaults the reader must ignore, and an engine field that could
    // cross by accident is one that could be stored by accident.
    AppConfig half;
    copyInterfaceHalf(cfg, half);
    json doc = json::parse(ConfigStore::serialize(half), nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) { return std::string(); }   // (ConfigStore::serialize always writes an object; this is a belt)
    for (auto it = doc.begin(); it != doc.end();) {
        if (interfaceKeys().count(it.key()) == 0) {
            it = doc.erase(it);
        } else {
            ++it;
        }
    }
    // The legacy single-window map rectangle: ConfigStore::serialize writes it only while mapPages is empty (the first real page entry
    // retires it from the FILE), but the window keeps it in memory as the default placement of a page with no entry of its own, so the
    // document carries it whenever it is set.
    if (half.mapWindowWidth > 0 && half.mapWindowHeight > 0 && doc.find("mapWindowWidth") == doc.end()) {
        doc["mapWindowWidth"] = half.mapWindowWidth;
        doc["mapWindowHeight"] = half.mapWindowHeight;
        doc["mapWindowX"] = half.mapWindowX;
        doc["mapWindowY"] = half.mapWindowY;
    }
    // error_handler_t::replace like every dump() in this tree (tests/test_json_dump_policy.cpp holds each site to it).
    return doc.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool parseInterfaceHalf(const std::string& document, AppConfig& out, std::string& error) {
    return ConfigStore::parse(document, "the interface half", out, error);
}

}  // namespace cascade::core
