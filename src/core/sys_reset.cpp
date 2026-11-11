// sys_reset.cpp - see sys_reset.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_reset.hpp"

#include <algorithm>
#include <cctype>

#include "core/sys_hash.hpp"

namespace fs = std::filesystem;

namespace cascade::core::sys {
namespace {

std::string lowerOf(std::string s) {
    for (auto& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool namedFoxsdr(const fs::path& p) {
    const fs::path name = p.filename().empty() ? p.parent_path().filename() : p.filename();   // tolerate a trailing separator
    return lowerOf(name.string()) == "foxsdr";
}

bool validStamp(const std::string& s) {
    if (s.empty() || s.size() > 32) { return false; }
    for (unsigned char c : s) { if (!(std::isalnum(c) || c == '-')) { return false; } }
    return true;
}

bool stampShape(const std::string& s) {   // YYYYMMDD-HHMMSS
    if (s.size() != 15) { return false; }
    for (std::size_t i = 0; i < 15; ++i) {
        if (i == 8) { if (s[i] != '-') { return false; } }
        else if (s[i] < '0' || s[i] > '9') { return false; }
    }
    return true;
}

void measure(const fs::path& dir, std::uint64_t& bytes, std::uint64_t& files) {
    std::error_code ec;
    bytes = 0;
    files = 0;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) {
            bytes += it->file_size(e2);
            ++files;
            if (files >= 200000) { break; }
        }
    }
}

std::string tokenOf(const std::vector<ResetItem>& items) {
    std::string s;
    for (const auto& it : items) {
        s += std::to_string(static_cast<int>(it.kind)) + "|" + it.relative + "|" + (it.isFolder ? "d" : "f") + "|" + std::to_string(it.bytes) + "|" + std::to_string(it.files) + "\n";
    }
    return "RESET-" + sha256Hex(s).substr(0, 8);
}

fs::path baseOf(const ResetItem& it, const fs::path& settingsDir, const fs::path& dataDir) { return it.kind == ResetKind::SettingsFile ? settingsDir : dataDir; }

}  // namespace

bool settingsFileNameIsOurs(const std::string& name) {
    for (const char* base : {"config.json", "bookmarks.json", "markers.json"}) {
        const std::string b = base;
        if (name == b) { return true; }
        // "<base>.<YYYYMMDD-HHMMSS>.bak": the shape the program's own automatic backups have
        if (name.size() == b.size() + 1 + 15 + 4 && name.compare(0, b.size(), b) == 0 && name[b.size()] == '.' && name.compare(name.size() - 4, 4, ".bak") == 0 &&
            stampShape(name.substr(b.size() + 1, 15))) {
            return true;
        }
    }
    return false;
}

ResetPlan planReset(const fs::path& settingsDir, const fs::path& dataDir, const ResetOptions& opt) {
    ResetPlan plan;
    if (settingsDir.empty() || !namedFoxsdr(settingsDir)) { plan.error = "the settings folder is not a folder named foxsdr; refusing"; return plan; }
    if ((opt.includePlugins || opt.includeLogs) && (dataDir.empty() || !namedFoxsdr(dataDir))) { plan.error = "the data folder is not a folder named foxsdr; refusing"; return plan; }
    std::error_code ec;
    if (fs::exists(settingsDir, ec)) {
        if (!fs::is_directory(settingsDir, ec)) { plan.error = "the settings path is not a folder"; return plan; }
        std::vector<ResetItem> found;
        for (auto it = fs::directory_iterator(settingsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            const std::string name = it->path().filename().string();
            if (it->is_regular_file(e2) && settingsFileNameIsOurs(name)) {
                ResetItem ri;
                ri.kind = ResetKind::SettingsFile;
                ri.relative = name;
                ri.bytes = it->file_size(e2);
                found.push_back(std::move(ri));
            } else {
                ++plan.leftAlone;
            }
        }
        if (ec) { plan.error = "the settings folder could not be listed"; plan.items.clear(); return plan; }
        std::sort(found.begin(), found.end(), [](const ResetItem& a, const ResetItem& b) { return a.relative < b.relative; });
        for (auto& f : found) { plan.items.push_back(std::move(f)); }
    }
    auto addFolder = [&](ResetKind kind, const char* name) {
        const fs::path p = dataDir / name;
        std::error_code e3;
        if (!fs::is_directory(p, e3)) { return; }
        ResetItem ri;
        ri.kind = kind;
        ri.relative = name;
        ri.isFolder = true;
        measure(p, ri.bytes, ri.files);
        plan.items.push_back(std::move(ri));
    };
    if (opt.includePlugins) { addFolder(ResetKind::PluginsFolder, "plugins"); }
    if (opt.includeLogs) { addFolder(ResetKind::LogsFolder, "logs"); }
    plan.confirmToken = tokenOf(plan.items);
    return plan;
}

bool executeReset(const fs::path& settingsDir, const fs::path& dataDir, const ResetOptions& opt, const std::string& token, const std::string& stamp, std::string& error) {
    error.clear();
    if (!validStamp(stamp)) { error = "the stamp is not a plain stamp"; return false; }
    const ResetPlan plan = planReset(settingsDir, dataDir, opt);
    if (!plan.error.empty()) { error = plan.error; return false; }
    if (token.empty() || token != plan.confirmToken) { error = "the confirmation does not match what is in the folders now"; return false; }
    if (plan.items.empty()) { return true; }   // nothing to reset
    std::error_code ec;
    const std::string aside = "reset-" + stamp;
    for (const fs::path& base : {settingsDir, dataDir}) {
        const fs::path d = base / aside;
        if (fs::exists(d, ec) && !fs::is_empty(d, ec)) { error = "a reset folder with that stamp already holds files"; return false; }
    }
    std::vector<std::pair<fs::path, fs::path>> moved;   // from, to
    auto rollback = [&] {
        std::error_code e2;
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) { fs::rename(it->second, it->first, e2); }
        for (const fs::path& base : {settingsDir, dataDir}) { fs::remove(base / aside, e2); }   // only if empty
    };
    for (const auto& item : plan.items) {
        const fs::path base = baseOf(item, settingsDir, dataDir);
        const fs::path from = base / item.relative;
        const fs::path toDir = base / aside;
        fs::create_directories(toDir, ec);
        if (ec) { error = "cannot make the reset folder"; rollback(); return false; }
        const fs::path to = toDir / item.relative;
        fs::rename(from, to, ec);
        if (ec) { error = "cannot move " + item.relative + " aside: " + ec.message(); rollback(); return false; }
        moved.emplace_back(from, to);
    }
    return true;
}

bool undoReset(const fs::path& settingsDir, const fs::path& dataDir, const std::string& stamp, std::string& error) {
    error.clear();
    if (!validStamp(stamp)) { error = "the stamp is not a plain stamp"; return false; }
    if (settingsDir.empty() || !namedFoxsdr(settingsDir)) { error = "the settings folder is not a folder named foxsdr; refusing"; return false; }
    const std::string aside = "reset-" + stamp;
    struct Move { fs::path from, to; };
    std::vector<Move> moves;
    std::error_code ec;
    for (const fs::path& base : {settingsDir, dataDir}) {
        if (base.empty()) { continue; }
        const fs::path d = base / aside;
        if (!fs::is_directory(d, ec)) { continue; }
        const bool isSettings = (base == settingsDir);
        for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            std::error_code e2;
            const bool okName = isSettings ? (it->is_regular_file(e2) && settingsFileNameIsOurs(name)) : (it->is_directory(e2) && (name == "plugins" || name == "logs"));
            if (!okName) { error = "the reset folder holds something that is not ours: " + name; return false; }
            moves.push_back({it->path(), base / name});
        }
    }
    if (moves.empty()) { error = "nothing was set aside under that stamp"; return false; }
    for (const auto& m : moves) {
        if (fs::exists(m.to, ec)) { error = m.to.filename().string() + " exists again; undo refused so nothing is overwritten"; return false; }
    }
    std::vector<Move> done;
    for (const auto& m : moves) {
        fs::rename(m.from, m.to, ec);
        if (ec) {
            error = "cannot restore " + m.to.filename().string() + ": " + ec.message();
            std::error_code e2;
            for (auto it = done.rbegin(); it != done.rend(); ++it) { fs::rename(it->to, it->from, e2); }
            return false;
        }
        done.push_back(m);
    }
    for (const fs::path& base : {settingsDir, dataDir}) { if (!base.empty()) { fs::remove(base / aside, ec); } }   // only if empty now
    return true;
}

}  // namespace cascade::core::sys
