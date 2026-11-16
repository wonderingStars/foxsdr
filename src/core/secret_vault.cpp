// secret_vault.cpp - see core/secret_vault.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/secret_vault.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core::vault {

namespace {

std::string hexOf(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) { out.push_back(d[c >> 4]); out.push_back(d[c & 15]); }
    return out;
}

bool bytesOf(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) { return false; }
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int a = nib(hex[i]);
        const int b = nib(hex[i + 1]);
        if (a < 0 || b < 0) { return false; }
        out.push_back(static_cast<char>((a << 4) | b));
    }
    return true;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { return std::string(); }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Temp file in the same folder, then a rename over the target (the way the settings writer does it).
bool writeAtomic(const fs::path& target, const std::string& text) {
    fs::path tmp = target;
    tmp += ".vtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { return false; }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

void tightenPermissions(const fs::path& p) {
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#else
    (void)p;
#endif
}

json readSidecar(const std::string& settingsPath) {
    const std::string text = slurp(sidecarPath(settingsPath));
    if (text.empty()) { return json::object(); }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("items") || !j["items"].is_object()) { return json::object(); }
    return j["items"];
}

bool writeSidecar(const std::string& settingsPath, const json& items) {
    const fs::path p(sidecarPath(settingsPath));
    if (items.empty()) {
        std::error_code ec;
        fs::remove(p, ec);
        return true;
    }
    json j;
    j["format"] = "foxsdr-secrets";
    j["version"] = 1;
    j["items"] = items;
    const bool ok = writeAtomic(p, j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
    if (ok) { tightenPermissions(p); }
    return ok;
}

std::string dumpDoc(const json& j) { return j.dump(4, ' ', false, json::error_handler_t::replace) + "\n"; }

std::mutex& declMutex() { static std::mutex m; return m; }
std::set<std::string>& declared() { static std::set<std::string> s; return s; }
std::string pluginItem(const std::string& plugin, const std::string& key) { return std::string("pluginSettings\x1f") + plugin + "\x1f" + key; }

bool keyNamesASecret(const std::string& key) {
    std::string k;
    for (const unsigned char c : key) { k.push_back(static_cast<char>(std::tolower(c))); }
    for (const char* frag : {"password", "passwd", "passphrase", "secret", "token", "apikey", "api_key", "privatekey", "private_key", "credential"}) {
        if (k.find(frag) != std::string::npos) { return true; }
    }
    return false;
}

// One value of the document that belongs in the protected store: the item's name and where it sits.
struct Slot {
    std::string item;
    json* value;
};

std::vector<Slot> slotsOf(json& j) {
    std::vector<Slot> out;
    for (const std::string& k : secretKeys()) {
        if (j.contains(k) && j[k].is_string()) { out.push_back({k, &j[k]}); }
    }
    if (j.contains("pluginSettings") && j["pluginSettings"].is_object()) {
        for (auto p = j["pluginSettings"].begin(); p != j["pluginSettings"].end(); ++p) {
            if (!p.value().is_object()) { continue; }
            for (auto e = p.value().begin(); e != p.value().end(); ++e) {
                if (e.value().is_string() && isPluginSecret(p.key(), e.key())) { out.push_back({pluginItem(p.key(), e.key()), &e.value()}); }
            }
        }
    }
    return out;
}

std::string labelOf(std::string item) {
    for (char& c : item) { if (c == '\x1f') { c = '/'; } }
    return item;
}

}  // namespace

const std::vector<std::string>& secretKeys() {
    static const std::vector<std::string> k = {"testerToken", "testerAppToken", "webPasswordRecord", "webListenerPasswordRecord"};
    return k;
}

void declarePluginSecret(const std::string& plugin, const std::string& key) {
    std::lock_guard<std::mutex> lk(declMutex());
    declared().insert(pluginItem(plugin, key));
}

bool isPluginSecret(const std::string& plugin, const std::string& key) {
    if (keyNamesASecret(key)) { return true; }
    std::lock_guard<std::mutex> lk(declMutex());
    return declared().count(pluginItem(plugin, key)) != 0;
}

void forgetDeclaredPluginSecretsForTest() {
    std::lock_guard<std::mutex> lk(declMutex());
    declared().clear();
}

std::string sidecarPath(const std::string& settingsPath) { return settingsPath + ".secrets"; }

bool hasProtectedStore() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool protectValue(const std::string& key, const std::string& plain, std::string& hexOut, std::string& why) {
    hexOut.clear();
    why.clear();
#ifdef _WIN32
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    const std::string ent = "foxsdr-settings-v1/" + key;
    DATA_BLOB entropy{static_cast<DWORD>(ent.size()), reinterpret_cast<BYTE*>(const_cast<char*>(ent.data()))};
    DATA_BLOB out{0, nullptr};
    // CRYPTPROTECT_UI_FORBIDDEN: a failure is an error return, never a window. Current-user scope (no LOCAL_MACHINE flag).
    if (!CryptProtectData(&in, L"FoxSDR setting", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        why = "the Windows protected store refused the value (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    hexOut = "dpapi:" + hexOf(std::string(reinterpret_cast<const char*>(out.pbData), out.cbData));
    LocalFree(out.pbData);
    return true;
#else
    (void)key;
    hexOut = "plain:" + hexOf(plain);
    return true;
#endif
}

bool unprotectValue(const std::string& key, const std::string& hexIn, std::string& plainOut, std::string& why) {
    plainOut.clear();
    why.clear();
    if (hexIn.rfind("plain:", 0) == 0) {
        if (!bytesOf(hexIn.substr(6), plainOut)) { why = "the stored value is damaged"; return false; }
        return true;
    }
    if (hexIn.rfind("dpapi:", 0) != 0) { why = "the stored value is in a form this build does not know"; return false; }
#ifdef _WIN32
    std::string blob;
    if (!bytesOf(hexIn.substr(6), blob)) { why = "the stored value is damaged"; return false; }
    DATA_BLOB in{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(blob.data())};
    const std::string ent = "foxsdr-settings-v1/" + key;
    DATA_BLOB entropy{static_cast<DWORD>(ent.size()), reinterpret_cast<BYTE*>(const_cast<char*>(ent.data()))};
    DATA_BLOB out{0, nullptr};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        why = "the Windows protected store cannot open the value for this user on this PC (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    plainOut.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
#else
    (void)key;
    why = "this build has no Windows protected store to open the value with";
    return false;
#endif
}

int scrubBackups(const std::string& settingsPath) {
    int scrubbed = 0;
    const fs::path target(settingsPath);
    const std::string stem = target.filename().string() + ".";
    const fs::path dir = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() != stem.size() + 15 + 4 || name.rfind(stem, 0) != 0 || name.compare(name.size() - 4, 4, ".bak") != 0) { continue; }
        const std::string text = slurp(e.path());
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { continue; }
        bool changed = false;
        for (Slot& sl : slotsOf(j)) {
            if (!sl.value->get<std::string>().empty()) { *sl.value = ""; changed = true; }
        }
        if (changed && writeAtomic(e.path(), dumpDoc(j))) { ++scrubbed; }
    }
    return scrubbed;
}

std::string splitForWrite(const std::string& settingsPath, const std::string& text, std::string& why, bool& kept) {
    why.clear();
    kept = false;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return text; }
    std::vector<Slot> slots = slotsOf(j);
    if (slots.empty()) { return text; }   // nothing of ours in this document
    json items = readSidecar(settingsPath);
    const json before = items;
    bool changedDoc = false;
    std::set<std::string> present;
    for (Slot& sl : slots) {
        present.insert(sl.item);
        const std::string value = sl.value->get<std::string>();
        if (value.empty()) {
            items.erase(sl.item);   // present and empty: the person cleared it
            continue;
        }
        std::string blob;
        std::string w;
        if (!protectValue(sl.item, value, blob, w)) {
            why = w;
            kept = true;      // nothing is lost: the value stays in the file
            continue;
        }
        items[sl.item] = blob;
        *sl.value = "";
        changedDoc = true;
    }
    // A plugin field that is no longer in the document at all (its settings were removed): its protected copy goes too.
    for (auto it = items.begin(); it != items.end();) {
        if (it.key().rfind("pluginSettings\x1f", 0) == 0 && present.count(it.key()) == 0) { it = items.erase(it); } else { ++it; }
    }
    if (items != before && !writeSidecar(settingsPath, items)) {
        why = "the protected copy could not be written beside the settings file";
        kept = true;
        return text;      // nothing was taken out of the text
    }
    return changedDoc ? dumpDoc(j) : text;
}

std::string mergeAfterRead(const std::string& settingsPath, const std::string& text, bool& migrated, std::string& note) {
    migrated = false;
    note.clear();
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return text; }
    json items = readSidecar(settingsPath);
    std::vector<Slot> slots = slotsOf(j);
    if (slots.empty() && items.empty()) { return text; }

    bool fileHadPlain = false;
    bool changedItems = false;
    std::vector<std::string> movedItems;
    auto addNote = [&](const std::string& item, const std::string& w) {
        note += (note.empty() ? "" : "; ") + labelOf(item) + ": " + w;   // names the field, never a value
    };
    for (Slot& sl : slots) {
        const std::string inFile = sl.value->get<std::string>();
        if (!inFile.empty()) {
            // A value still in the file as text: it wins (an older build wrote it, or a person pasted it) and is migrated.
            std::string blob;
            std::string w;
            if (protectValue(sl.item, inFile, blob, w)) { items[sl.item] = blob; changedItems = true; fileHadPlain = true; movedItems.push_back(sl.item); }
            else { addNote(sl.item, w); }
            continue;
        }
        if (items.contains(sl.item) && items[sl.item].is_string()) {
            std::string plain;
            std::string w;
            if (unprotectValue(sl.item, items[sl.item].get<std::string>(), plain, w)) { *sl.value = plain; }
            else { addNote(sl.item, w); }
        }
    }
    if (fileHadPlain) {
        // The file is rewritten without the plain values, once, and the older copies are scrubbed.
        bool ok = changedItems ? writeSidecar(settingsPath, items) : true;
        if (ok) {
            json stripped = json::parse(text, nullptr, false);
            for (Slot& sl : slotsOf(stripped)) {
                for (const std::string& m : movedItems) { if (m == sl.item) { *sl.value = ""; } }
            }
            ok = writeAtomic(fs::path(settingsPath), dumpDoc(stripped));
            if (ok) { scrubBackups(settingsPath); migrated = true; }
        }
        if (!ok) { note += (note.empty() ? "" : "; ") + std::string("the settings file could not be rewritten without its secrets"); }
    }
    return dumpDoc(j);
}

void forget(const std::string& settingsPath) {
    std::error_code ec;
    fs::remove(sidecarPath(settingsPath), ec);
}

}  // namespace cascade::core::vault
