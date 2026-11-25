// consent_file.cpp - see consent_file.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/consent_file.hpp"

#include <filesystem>

#include <nlohmann/json.hpp>

#include "core/config.hpp"

namespace cascade::core::consent {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

std::mutex& hookMutex() {
    static std::mutex m;
    return m;
}
const void*& hookOwner() {
    static const void* o = nullptr;
    return o;
}
std::function<bool(const std::string&)>& hookFn() {
    static std::function<bool(const std::string&)> f;
    return f;
}

std::string dumpLikeSerialize(const json& j) { return j.dump(4, ' ', false, json::error_handler_t::replace) + "\n"; }

}  // namespace

const std::vector<std::string>& usageKeys() {
    static const std::vector<std::string> k = {
        "telemetryEnabled",     "telemetryInstallId",   "telemetryLaunches",    "telemetryCrashes",    "telemetryExitsDied",
        "telemetryExitsKilled", "telemetryExitsEnded",  "telemetryExitsUnknown", "telemetryFirstRun",  "telemetryFirstVersion",
        "telemetryPending"};
    return k;
}

const std::vector<std::string>& switchKeys() {
    static const std::vector<std::string> k = {"diagnosticsEnabled", "updateCheckEnabled"};
    return k;
}

const std::vector<std::string>& testerKeys() {
    static const std::vector<std::string> k = {"testerUsageCurrent", "testerUsagePending"};
    return k;
}

std::mutex& ioMutex() {
    static std::mutex m;
    return m;
}

bool readFields(const std::string& path, Fields& out) {
    out = Fields{};
    AppConfig c;
    std::string err;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(fs::u8path(path), ec)) { return false; }
    if (!ConfigStore::load(path, c, err)) { return false; }
    out.ok = true;
    out.usageEnabled = c.telemetryEnabled;
    out.installId = c.telemetryInstallId;
    out.diagnostics = c.diagnosticsEnabled;
    out.updateCheck = c.updateCheckEnabled;
    return true;
}

std::string overlay(const std::string& text, const std::string& path, Role role) {
    AppConfig disk;
    std::string err;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(fs::u8path(path), ec) || !ConfigStore::load(path, disk, err)) { return text; }
    json mine = json::parse(text, nullptr, false);
    if (mine.is_discarded() || !mine.is_object()) { return text; }
    const json theirs = json::parse(ConfigStore::serialize(disk), nullptr, false);
    if (theirs.is_discarded() || !theirs.is_object()) { return text; }
    auto lay = [&](const std::vector<std::string>& keys) {
        for (const std::string& k : keys) {
            const auto it = theirs.find(k);
            if (it != theirs.end()) { mine[k] = *it; }
        }
    };
    if (role == Role::Second) {
        lay(usageKeys());
        lay(switchKeys());
        lay(testerKeys());
    } else {
        const std::string memoryId = mine.value("telemetryInstallId", std::string());
        const bool pendingStands = disk.telemetryEnabled && !disk.telemetryInstallId.empty() && disk.telemetryInstallId == memoryId;
        mine["telemetryEnabled"] = disk.telemetryEnabled;
        mine["telemetryInstallId"] = disk.telemetryInstallId;
        lay(switchKeys());
        if (!pendingStands) { mine["telemetryPending"] = std::string(); }
    }
    return dumpLikeSerialize(mine);
}

bool writeGuarded(const std::string& path, const std::string& text, Role role, std::string& error) {
    std::lock_guard<std::mutex> lk(ioMutex());
    return ConfigStore::writeFile(path, overlay(text, path, role), error);
}

std::function<bool(const std::string&, const std::string&, std::string&)> guardedWriter(std::shared_ptr<std::atomic<int>> role) {
    return [role](const std::string& path, const std::string& text, std::string& error) {
        const int r = role->load();
        if (r < 0) { return ConfigStore::writeFile(path, text, error); }
        return writeGuarded(path, text, r == 0 ? Role::Holder : Role::Second, error);
    };
}

bool mergeKeys(const std::string& path, const std::string& sourceText, const std::vector<std::string>& keys, std::string& error) {
    std::lock_guard<std::mutex> lk(ioMutex());
    error.clear();
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(path), ec)) { return ConfigStore::writeFile(path, sourceText, error); }
    AppConfig disk;
    if (!ConfigStore::load(path, disk, error)) {
        error = "the settings file could not be read, so the switch was not written into it: " + error;
        return false;
    }
    json base = json::parse(ConfigStore::serialize(disk), nullptr, false);
    const json src = json::parse(sourceText, nullptr, false);
    if (base.is_discarded() || !base.is_object() || src.is_discarded() || !src.is_object()) {
        error = "the switch could not be merged into the settings file";
        return false;
    }
    for (const std::string& k : keys) {
        const auto it = src.find(k);
        if (it != src.end()) { base[k] = *it; }
    }
    return ConfigStore::writeFile(path, dumpLikeSerialize(base), error);
}

bool Watch::read(Fields& out, bool force) {
    std::lock_guard<std::mutex> lk(m_);
    std::error_code ec;
    const fs::path p = fs::u8path(path_);
    const std::uintmax_t size = fs::is_regular_file(p, ec) ? fs::file_size(p, ec) : 0;
    const auto t = fs::last_write_time(p, ec);
    const std::int64_t time = ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
    if (have_ && !force && size == size_ && time == time_) {
        out = cached_;
        return false;
    }
    Fields f;
    (void)readFields(path_, f);
    // a file that could not be read this time keeps what was known (a write was under way); the stamp is not taken so the next look tries again
    if (f.ok || !have_) {
        cached_ = f;
        size_ = static_cast<std::uint64_t>(size);
        time_ = time;
        have_ = true;
    }
    out = cached_;
    return true;
}

void setSendHook(const void* owner, std::function<bool(const std::string&)> allow) {
    std::lock_guard<std::mutex> lk(hookMutex());
    hookOwner() = owner;
    hookFn() = std::move(allow);
}

void clearSendHook(const void* owner) {
    std::lock_guard<std::mutex> lk(hookMutex());
    if (hookOwner() == owner) {
        hookOwner() = nullptr;
        hookFn() = nullptr;
    }
}

bool sendVetoed(const std::string& url) {
    // The hook is called with the mutex held: clearSendHook (the engine's destructor) then waits for a send's look that is under way, instead of leaving it to run into a dead engine.
    std::lock_guard<std::mutex> lk(hookMutex());
    return hookFn() && !hookFn()(url);
}

}  // namespace cascade::core::consent
