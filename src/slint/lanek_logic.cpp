// lanek_logic.cpp - see lanek_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanek_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/i18n.hpp"
#include "core/sys_hash.hpp"

namespace foxsdr::slintlanek {

namespace sys = cascade::core::sys;
using nlohmann::json;


// The first-run setup's choice labels and starter titles come from core/sys_first_run.cpp as plain text; this list is how the string tables learn them (tools/i18n_keys.py reads FOX_TR_NOOP).
const std::vector<const char*>& wizardStrings() {
    static const std::vector<const char*> v = {
        FOX_TR_NOOP("I have no radio yet (try the built-in test signal)"), FOX_TR_NOOP("RTL-SDR dongle"), FOX_TR_NOOP("Airspy R2 / Mini"), FOX_TR_NOOP("Airspy HF+"), FOX_TR_NOOP("SDRplay RSP"),
        FOX_TR_NOOP("HackRF"), FOX_TR_NOOP("RX888 mk2"), FOX_TR_NOOP("Mirics MSi2500"), FOX_TR_NOOP("ADALM-Pluto"), FOX_TR_NOOP("A recorded I/Q file"), FOX_TR_NOOP("A sound card input"),
        FOX_TR_NOOP("FM broadcast band"), FOX_TR_NOOP("Airband emergency channel"), FOX_TR_NOOP("Marine VHF channel 16"), FOX_TR_NOOP("PMR446 channel 1"), FOX_TR_NOOP("2 m amateur FM"),
        FOX_TR_NOOP("Time signals (WWV)"), FOX_TR_NOOP("Shortwave broadcast, 49 m"), FOX_TR_NOOP("Amateur HF")};
    return v;
}

// ============================================================ rows ============================================================
Row rHeading(const std::string& label) { Row r; r.kind = kHeading; r.label = label; return r; }
Row rNote(const std::string& text, int tone) { Row r; r.kind = kNote; r.label = text; r.tone = tone; return r; }
Row rKey(const std::string& id, const std::string& label, bool enabled, const std::string& tip, bool on) { Row r; r.kind = kKey; r.id = id; r.label = label; r.enabled = enabled; r.note = tip; r.on = on; return r; }
Row rTick(const std::string& id, const std::string& label, bool on, bool enabled, const std::string& tip) { Row r; r.kind = kTick; r.id = id; r.label = label; r.on = on; r.enabled = enabled; r.note = tip; return r; }
Row rChoice(const std::string& id, const std::string& label, const std::vector<std::string>& options, int pick, bool enabled, const std::string& tip) {
    Row r; r.kind = kChoice; r.id = id; r.label = label; r.options = options; r.pick = pick; r.enabled = enabled; r.note = tip; return r;
}
Row rField(const std::string& id, const std::string& label, const std::string& value, const std::string& tip, bool enabled) { Row r; r.kind = kField; r.id = id; r.label = label; r.value = value; r.note = tip; r.enabled = enabled; return r; }
Row rReadout(const std::string& label, const std::string& value) { Row r; r.kind = kReadout; r.label = label; r.value = value; return r; }
Row rSlider(const std::string& id, const std::string& label, float level, const std::string& readout, bool enabled, const std::string& tip) {
    Row r; r.kind = kSlider; r.id = id; r.label = label; r.level = std::clamp(level, 0.0f, 1.0f); r.value = readout; r.enabled = enabled; r.note = tip; return r;
}
Row rBar(const std::string& label, float level, const std::string& value) { Row r; r.kind = kBar; r.label = label; r.level = std::clamp(level, 0.0f, 1.0f); r.value = value; return r; }

std::string signatureOf(const std::vector<Row>& rows) {
    std::string s;
    for (const Row& r : rows) {
        s += std::to_string(r.kind); s += '|'; s += r.id; s += '|'; s += r.label; s += '|'; s += r.value; s += '|'; s += r.note; s += '|';
        s += r.on ? '1' : '0'; s += r.enabled ? '1' : '0'; s += std::to_string(r.pick); s += '|';
        s += std::to_string(static_cast<int>(std::lround(r.level * 1000.0f))); s += '|'; s += std::to_string(r.tone);
        for (const auto& o : r.options) { s += '|'; s += o; }
        s += '\n';
    }
    return s;
}

// ============================================================ the record ============================================================
namespace {
constexpr std::size_t kMaxRecordBytes = 256 * 1024;
constexpr std::size_t kMaxKeys = 512;
bool keyOk(const std::string& k) { return !k.empty() && k.size() <= 64; }
}  // namespace

bool Record::loadText(const std::string& text) {
    kv_.clear();
    dirty_ = false;
    if (text.empty() || text.size() > kMaxRecordBytes) { return false; }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return false; }
    for (auto it = j.begin(); it != j.end() && kv_.size() < kMaxKeys; ++it) {
        if (!keyOk(it.key())) { continue; }
        kv_[it.key()] = it.value().dump(-1, ' ', false, json::error_handler_t::replace);
    }
    return true;
}

std::string Record::toText() const {
    json j = json::object();
    for (const auto& [k, v] : kv_) {
        const json x = json::parse(v, nullptr, false);
        if (!x.is_discarded()) { j[k] = x; }
    }
    return j.dump(1, ' ', false, json::error_handler_t::replace);
}

static json valueOf(const std::map<std::string, std::string>& kv, const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end()) { return json(); }
    const json v = json::parse(it->second, nullptr, false);
    return v.is_discarded() ? json() : v;
}

bool Record::getBool(const std::string& key, bool def) const {
    const json v = valueOf(kv_, key);
    return v.is_boolean() ? v.get<bool>() : def;
}
long long Record::getInt(const std::string& key, long long def, long long lo, long long hi) const {
    const json v = valueOf(kv_, key);
    long long x = def;
    if (v.is_number_integer()) { x = v.get<long long>(); }
    else if (v.is_number_float() && std::isfinite(v.get<double>())) { x = static_cast<long long>(std::llround(v.get<double>())); }
    return std::clamp(x, lo, hi);
}
double Record::getNum(const std::string& key, double def, double lo, double hi) const {
    const json v = valueOf(kv_, key);
    double x = def;
    if (v.is_number() && std::isfinite(v.get<double>())) { x = v.get<double>(); }
    return std::clamp(x, lo, hi);
}
std::string Record::getStr(const std::string& key, const std::string& def, std::size_t maxLen) const {
    const json v = valueOf(kv_, key);
    if (!v.is_string()) { return def; }
    std::string s = v.get<std::string>();
    if (s.size() > maxLen) { s.resize(maxLen); }
    return s;
}

static bool setRaw(std::map<std::string, std::string>& kv, bool& dirty, const std::string& key, const json& v) {
    if (!keyOk(key)) { return false; }
    const std::string text = v.dump(-1, ' ', false, json::error_handler_t::replace);
    const auto it = kv.find(key);
    if (it != kv.end() && it->second == text) { return false; }
    if (it == kv.end() && kv.size() >= kMaxKeys) { return false; }
    kv[key] = text;
    dirty = true;
    return true;
}
bool Record::setBool(const std::string& key, bool v) { return setRaw(kv_, dirty_, key, json(v)); }
bool Record::setInt(const std::string& key, long long v) { return setRaw(kv_, dirty_, key, json(v)); }
bool Record::setNum(const std::string& key, double v) { return std::isfinite(v) && setRaw(kv_, dirty_, key, json(v)); }
bool Record::setStr(const std::string& key, const std::string& v) { return setRaw(kv_, dirty_, key, json(v.size() > 2048 ? v.substr(0, 2048) : v)); }

std::map<std::string, std::string> Record::getMap(const std::string& key) const {
    std::map<std::string, std::string> out;
    const json v = valueOf(kv_, key);
    if (!v.is_object()) { return out; }
    for (auto it = v.begin(); it != v.end() && out.size() < 128; ++it) {
        if (it.value().is_string() && it.key().size() <= 64) { out[it.key()] = it.value().get<std::string>(); }
    }
    return out;
}
void Record::setMap(const std::string& key, const std::map<std::string, std::string>& m) {
    json o = json::object();
    std::size_t n = 0;
    for (const auto& [k, v] : m) { if (++n > 128) { break; } o[k] = v; }
    setRaw(kv_, dirty_, key, o);
}

// ============================================================ the settings keeper ============================================================
namespace {
constexpr const char* kBundleExt = ".foxsdr-bundle";

bool slurpFile(const fs::path& p, std::size_t cap, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return false; }
    const auto sz = fs::file_size(p, ec);
    if (ec || sz > cap) { return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeAtomic(const fs::path& target, const std::string& bytes, std::string& err) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) { err = "cannot make the folder"; return false; }
    fs::path tmp = target;
    tmp += ".part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        f.flush();
        if (!f) { err = "cannot write the file"; fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, target, ec);
    if (ec) { err = "cannot put the file in place"; fs::remove(tmp, ec); return false; }
    return true;
}

fs::path pathOf(const std::string& u8) { return fs::path(std::u8string(u8.begin(), u8.end())); }
}  // namespace

std::string stampOf(std::int64_t unixSeconds) {
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[32] = {};
    std::snprintf(b, sizeof(b), "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

bool SettingsKeeper::readCurrent(std::vector<sys::BundleFile>& out, std::string& err) const {
    out.clear();
    err.clear();
    for (const char* name : {"config.json", "bookmarks.json", "markers.json"}) {
        std::string bytes;
        if (!slurpFile(d_.settings / name, sys::kBundleMaxFileBytes, bytes)) { continue; }
        out.push_back({name, std::move(bytes)});
    }
    if (out.empty()) { err = "there is no settings file yet"; return false; }
    return true;
}

ExportOut SettingsKeeper::exportTo(const fs::path& dir, const std::string& stamp, const std::string& appVersion) const {
    ExportOut o;
    std::vector<sys::BundleFile> files;
    if (!readCurrent(files, o.error)) { return o; }
    sys::ExportOptions opt;
    opt.stripIdentity = true;
    opt.appVersion = appVersion;
    opt.createdUtc = stamp;
    sys::ExportReport rep;
    const std::string text = sys::exportBundle(files, opt, rep);
    if (rep.error != sys::BundleError::None) { o.error = std::string(sys::bundleErrorName(rep.error)) + " " + rep.detail; return o; }
    const fs::path target = dir / sys::backupFileName(stamp);
    if (!writeAtomic(target, text, o.error)) { return o; }
    o.ok = true;
    o.path = target.string();
    o.stripped = static_cast<int>(rep.stripped.size());
    return o;
}

ExportOut SettingsKeeper::backupNow(const std::string& stamp, const std::string& appVersion, int keep) const {
    ExportOut o = exportTo(d_.backups, stamp, appVersion);
    if (o.ok) { sys::pruneBackups(d_.backups, static_cast<std::size_t>(std::clamp(keep, 1, 50))); }
    return o;
}

std::vector<std::string> SettingsKeeper::listBundles(const fs::path& dir, std::size_t limit) const {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) { continue; }
        const std::string n = it->path().filename().string();
        if (n.size() > std::string(kBundleExt).size() && n.compare(n.size() - std::string(kBundleExt).size(), std::string::npos, kBundleExt) == 0) { v.push_back(n); }
    }
    std::sort(v.begin(), v.end(), std::greater<>());
    if (v.size() > limit) { v.resize(limit); }
    return v;
}

ImportPreview SettingsKeeper::previewImport(const fs::path& file) const {
    ImportPreview p;
    std::string text;
    if (!slurpFile(file, sys::kBundleMaxTextBytes, text)) { p.error = "the file cannot be read, or it is too large"; return p; }
    p.res = sys::parseBundle(text);
    if (p.res.error != sys::BundleError::None) { p.error = std::string(sys::bundleErrorName(p.res.error)) + " " + p.res.detail; return p; }
    p.plan = sys::planImport(p.res, d_.settings);
    for (const auto& it : p.plan.items) {
        if (it.action == sys::ImportAction::Replace) { ++p.replace; }
        else if (it.action == sys::ImportAction::New) { ++p.added; }
        else { ++p.identical; }
    }
    p.ok = true;
    return p;
}

std::vector<std::string> SettingsKeeper::profileNames() const {
    std::vector<std::string> v;
    for (const auto& p : sys::listProfiles(d_.profiles)) { v.push_back(p.name); }
    return v;
}
bool SettingsKeeper::saveProfile(const std::string& name, bool overwrite, const std::string& appVersion, std::string& err) const {
    std::vector<sys::BundleFile> files;
    if (!readCurrent(files, err)) { return false; }
    sys::ExportOptions opt;
    opt.appVersion = appVersion;
    return sys::saveProfile(d_.profiles, name, files, overwrite, opt, err);
}
bool SettingsKeeper::deleteProfile(const std::string& name, std::string& err) const { return sys::deleteProfile(d_.profiles, name, err); }
sys::SwitchPlan SettingsKeeper::planSwitch(const std::string& name) const { return sys::planSwitch(d_.profiles, name, d_.settings); }

sys::ResetPlan SettingsKeeper::planReset(const sys::ResetOptions& o) const { return sys::planReset(d_.settings, d_.data, o); }

std::vector<std::string> SettingsKeeper::resetStamps() const {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(d_.settings, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) { continue; }
        const std::string n = it->path().filename().string();
        if (n.rfind("reset-", 0) == 0 && n.size() == 6 + 15) { v.push_back(n.substr(6)); }
    }
    std::sort(v.begin(), v.end(), std::greater<>());
    return v;
}

SyncReport SettingsKeeper::sync(const fs::path& shared, std::map<std::string, std::string>& base) const {
    SyncReport r;
    std::error_code ec;
    if (shared.empty() || !fs::is_directory(shared, ec)) { r.error = "the shared folder is not there"; return r; }
    const auto entries = [&](const fs::path& dir) {
        std::vector<sys::SyncEntry> v;
        for (const auto& p : sys::listProfiles(dir)) {
            sys::SyncEntry e;
            e.name = p.name;
            e.sha256 = p.sha256;
            v.push_back(e);
        }
        return v;
    };
    const auto local = entries(d_.profiles);
    const auto remote = entries(shared);
    const auto plan = sys::planSync(local, remote, base);
    const auto copyFile = [&](const fs::path& from, const fs::path& to) {
        std::error_code e;
        fs::create_directories(to.parent_path(), e);
        fs::path tmp = to;
        tmp += ".part";
        fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, e);
        if (e) { return false; }
        fs::rename(tmp, to, e);
        return !e;
    };
    const auto shaOf = [&](const std::vector<sys::SyncEntry>& v, const std::string& n) { for (const auto& x : v) { if (x.name == n) { return x.sha256; } } return std::string(); };
    for (const auto& it : plan) {
        const std::string file = it.name + ".foxsdr-profile";
        switch (it.action) {
            case sys::SyncAction::PushToShared:
                if (copyFile(d_.profiles / file, shared / file)) { ++r.pushed; base[it.name] = shaOf(local, it.name); }
                break;
            case sys::SyncAction::PullFromShared:
                if (copyFile(shared / file, d_.profiles / file)) { ++r.pulled; base[it.name] = shaOf(remote, it.name); }
                break;
            case sys::SyncAction::Conflict: ++r.conflicts; r.conflictNames.push_back(it.name); break;
            case sys::SyncAction::None: ++r.same; if (!shaOf(local, it.name).empty()) { base[it.name] = shaOf(local, it.name); } break;
        }
    }
    r.ok = true;
    return r;
}

bool SettingsKeeper::apply(const Pending& p, std::string& detail) const {
    detail.clear();
    switch (p.op) {
        case Op::None: return true;
        case Op::Import: {
            const sys::ImportPlan now = sys::planImport(p.staged, d_.settings);
            return sys::commitImport(p.staged, d_.settings, now.confirmToken, p.stamp, detail);
        }
        case Op::Switch: {
            sys::SwitchPlan sw = sys::planSwitch(d_.profiles, p.profile, d_.settings);
            if (!sw.error.empty()) { detail = sw.error; return false; }
            return sys::applySwitch(sw, d_.settings, sw.plan.confirmToken, p.stamp, detail);
        }
        case Op::Reset: {
            const sys::ResetPlan now = sys::planReset(d_.settings, d_.data, p.reset);
            if (!now.error.empty()) { detail = now.error; return false; }
            if (now.items.size() < 1) { return true; }
            return sys::executeReset(d_.settings, d_.data, p.reset, now.confirmToken, p.stamp, detail);
        }
        case Op::UndoReset: return sys::undoReset(d_.settings, d_.data, p.undoStamp, detail);
    }
    return false;
}

}  // namespace foxsdr::slintlanek
