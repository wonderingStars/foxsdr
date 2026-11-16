// radio_reporter.cpp - see radio_reporter.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_reporter.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/crash_upload.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core::radioreport {

namespace {

constexpr std::size_t kMaxFailBytes = 16384;
constexpr std::size_t kMaxFirstSessionBytes = 8192;
constexpr std::size_t kMaxJournalBytes = 4096;
constexpr std::int64_t kMaxT = 2592000;

fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string dumpJson(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { return std::string(); }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Appends `data` and forces it to the disk before returning: an event that comes before a call that can hang or kill the process is on disk
// when that call is made (contract 3.9, C2).
bool appendDurable(const fs::path& p, const std::string& data) {
#if defined(_WIN32)
    HANDLE h = ::CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DWORD wrote = 0;
    const BOOL ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
    const BOOL synced = ::FlushFileBuffers(h);
    ::CloseHandle(h);
    return ok && synced && wrote == data.size();
#else
    const int fd = ::open(p.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) { return false; }
    const ssize_t n = ::write(fd, data.data(), data.size());
    const int synced = ::fsync(fd);
    ::close(fd);
    return n == static_cast<ssize_t>(data.size()) && synced == 0;
#endif
}

// Replaces the file whole: a temporary file, forced to disk, then renamed over it.
bool writeAtomic(const fs::path& p, const std::string& data) {
    fs::path tmp = p;
    tmp += ".tmp";
    std::error_code ec;
    fs::remove(tmp, ec);
    if (!appendDurable(tmp, data)) { return false; }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        std::error_code ec2;
        fs::rename(tmp, p, ec2);
        return !ec2;
    }
    return true;
}

std::string dayOf(std::int64_t epoch) {
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

std::int64_t systemNow() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

bool textRefusalCode(const std::string& code) {
    return code == "path-in-text" || code == "address-in-text" || code == "serial-in-text" || code == "frequency-in-text";
}

// The contract's section 4, verbatim: a key matching it refuses the whole message, whatever its value.
bool forbiddenKey(const std::string& keyIn) {
    static const std::regex kForbidden(
        "freq|hertz|hz$|centre|center|tuned|bookmark|position|latitude|longitude|gps|ipaddr|hostname|username|filename|folder|decoded|callsign|password|secret|^(lat|lon|lng|ip|user|path|file|token)$");
    std::string k = keyIn;
    for (char& c : k) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return std::regex_search(k, kForbidden);
}

bool hasForbiddenKey(const json& j) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (forbiddenKey(it.key()) || hasForbiddenKey(it.value())) { return true; }
        }
    } else if (j.is_array()) {
        for (const json& x : j) { if (hasForbiddenKey(x)) { return true; } }
    }
    return false;
}

// The fields of a journal body the text rules apply to (the Worker's `text` and `model` types, plus firmware and library).
const std::set<std::string>& textFieldNames() {
    static const std::set<std::string> s = {"model", "reason", "antenna", "usbHost", "cpu", "gpu", "usbHosts", "sdr", "firmware", "library"};
    return s;
}

// Defence in depth over EVERY string in a body: a path or a hertz number is never acceptable anywhere (an address is checked only on the text
// fields, where a four-part version cannot be mistaken for one).
bool stringHoldsForbiddenText(const std::string& s) { return pathInText(s) || frequencyInText(s); }

bool anyStringForbidden(const json& j) {
    if (j.is_string()) { return stringHoldsForbiddenText(j.get<std::string>()); }
    if (j.is_object()) { for (auto it = j.begin(); it != j.end(); ++it) { if (anyStringForbidden(it.value())) { return true; } } }
    if (j.is_array()) { for (const json& x : j) { if (anyStringForbidden(x)) { return true; } } }
    return false;
}

bool inList(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

const std::vector<std::string>& kindsOfJournal() {
    static const std::vector<std::string> k = {"radio_found", "radio_open", "radio_settings", "radio_stream", "radio_recovery", "radio_close", "session_end", "session_env"};
    return k;
}

std::string fieldKey(const std::string& radioKey, const std::string& kind, const std::string& word) { return radioKey + "|" + kind + "|" + word; }

// A message the sender will post, with what is needed to take it off the queue afterwards.
struct Item {
    bool isFail = false;
    fs::path file;            // the failure file, or the journal segment
    std::size_t line = 0;     // the journal line's index (0-based, after the header)
    std::string kind;
    std::string text;         // the JSON to post
    std::string radioKey;
    bool degraded = false;
    bool anonymous = false;
    std::int64_t eventEpoch = 0;
    json env;                 // the message as stored (without `age`)
};

}  // namespace

const char* triggerWord(Trigger t) {
    switch (t) {
        case Trigger::StartupRestore: return "startup-restore";
        case Trigger::UserPick: return "user-pick";
        case Trigger::SavedRetry: return "saved-retry";
        case Trigger::PatchRadio: return "patch-radio";
        case Trigger::AutoReopen: return "auto-reopen";
        case Trigger::Other: break;
    }
    return "other";
}

Event& Event::text(const std::string& name, const std::string& raw, std::size_t maxChars) {
    std::string serial = radio.serial;
    fields.emplace_back(name, FieldValue(scrubText(raw, serial, maxChars)));
    return *this;
}

Transport makeRealTransport(const std::shared_ptr<UploadCancel>& cancel) {
    return [cancel](const std::string& url, const std::string& body) {
        PostResult out;
        const RawPostResult r = postBounded(url, body, cancel, /*captureBody=*/true);
        out.attempted = r.attempted;
        out.status = r.status;
        out.retryAfterSec = r.retryAfterSeconds;
        out.body = r.body;
        return out;
    };
}

// ===========================================================================
// The reporter
// ===========================================================================

struct Reporter::Impl {
    Options o;
    bool on = false;
    fs::path dir;
    std::shared_ptr<UploadCancel> cancel = std::make_shared<UploadCancel>();

    mutable std::mutex mu;       // all state and all file work
    std::mutex batchMu;          // one batch at a time
    Identity id;
    std::string sessionId;
    std::int64_t sessionStart = 0;
    std::uint64_t seq = 0;
    Stats st;

    // the day's bound state (persisted)
    std::string day;
    int failAdmitted = 0;
    int journalAdmitted = 0;
    std::set<std::string> firstSeen;                 // "key|class" and "key|kind|word"
    std::map<std::string, long long> dropped;        // per radio key, not yet said on a sent event
    std::map<std::string, long long> streamToday;    // per radio key
    long long refusedTotal = 0;

    struct Pending {
        std::string kind, key;
        json body;
        std::int64_t t = 0;
        std::int64_t atEpoch = 0;
        std::string cls;      // reasonClass for failures
        bool isFail = false;
    };
    std::map<std::string, Pending> held;

    // per process
    std::map<std::string, long long> failAttempt, openAttempt, recoveryAttempt;
    std::map<std::string, std::int64_t> lastStreamAt;
    std::vector<std::string> openKeys;
    bool envSent = false;
    int firstSessionSent = 0;
    std::int64_t firstSessionLastAt = -1;

    // journal segment being written
    int segNo = 0;
    std::size_t segBytes = 0;
    bool segReady = false;
    std::size_t appendsSinceSweep = 0;

    // sender
    std::thread worker;
    std::atomic<bool> running{false};
    std::int64_t nextBatchAt = 0;
    std::int64_t backoffUntil = 0;
    int failStreak = 0;
    bool urgent = false;
    bool configuredOnce = false;
    struct PreFail { FailureInput in; std::vector<std::string> ring; std::int64_t at; };
    struct PreEvent { Event ev; std::int64_t at; };
    std::vector<PreFail> preFails;
    std::vector<PreEvent> preEvents;

    std::int64_t now() const { return o.nowEpoch ? o.nowEpoch() : systemNow(); }

    // ------------------------------------------------------------------ state file
    fs::path statePath() const { return dir / "state.json"; }

    void saveState() {
        json j;
        j["v"] = 1;
        j["day"] = day;
        j["failAdmitted"] = failAdmitted;
        j["journalAdmitted"] = journalAdmitted;
        j["firstSeen"] = json(firstSeen);
        j["dropped"] = json(dropped);
        j["streamToday"] = json(streamToday);
        j["refused"] = refusedTotal;
        json h = json::object();
        for (const auto& kv : held) {
            json e;
            e["kind"] = kv.second.kind;
            e["key"] = kv.second.key;
            e["body"] = kv.second.body;
            e["t"] = kv.second.t;
            e["at"] = kv.second.atEpoch;
            e["cls"] = kv.second.cls;
            e["fail"] = kv.second.isFail;
            h[kv.first] = std::move(e);
        }
        j["held"] = std::move(h);
        if (!writeAtomic(statePath(), dumpJson(j))) { ++st.guardRefused; }
    }

    void loadState() {
        const std::string text = readAll(statePath());
        if (text.empty()) { return; }
        const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) { return; }
        day = j.value("day", std::string());
        failAdmitted = j.value("failAdmitted", 0);
        journalAdmitted = j.value("journalAdmitted", 0);
        refusedTotal = j.value("refused", 0LL);
        if (j.contains("firstSeen") && j["firstSeen"].is_array()) {
            for (const json& x : j["firstSeen"]) { if (x.is_string()) { firstSeen.insert(x.get<std::string>()); } }
        }
        if (j.contains("dropped") && j["dropped"].is_object()) {
            for (auto it = j["dropped"].begin(); it != j["dropped"].end(); ++it) { if (it.value().is_number()) { dropped[it.key()] = it.value().get<long long>(); } }
        }
        if (j.contains("streamToday") && j["streamToday"].is_object()) {
            for (auto it = j["streamToday"].begin(); it != j["streamToday"].end(); ++it) { if (it.value().is_number()) { streamToday[it.key()] = it.value().get<long long>(); } }
        }
        if (j.contains("held") && j["held"].is_object()) {
            for (auto it = j["held"].begin(); it != j["held"].end(); ++it) {
                const json& e = it.value();
                if (!e.is_object() || !e.contains("body")) { continue; }
                Pending p;
                p.kind = e.value("kind", std::string());
                p.key = e.value("key", std::string());
                p.body = e["body"];
                p.t = e.value("t", 0LL);
                p.atEpoch = e.value("at", 0LL);
                p.cls = e.value("cls", std::string());
                p.isFail = e.value("fail", false);
                held[it.key()] = std::move(p);
            }
        }
    }

    // ------------------------------------------------------------------ envelope
    json envelopeFor(const Pending& p, bool anonymousFail) {
        json e;
        e["schema"] = kSchema;
        e["kind"] = p.kind;
        if (anonymousFail) {
            e["messageId"] = randomHex(16);
        } else {
            e["installId"] = id.installId;
            e["sessionId"] = sessionId;
            e["seq"] = seq++;
        }
        e["t"] = std::clamp<std::int64_t>(p.t, 0, kMaxT);
        e["radioKey"] = p.key;
        e["body"] = p.body;
        return e;
    }

    // ------------------------------------------------------------------ folder bounds
    struct Seg {
        fs::path path;
        std::int64_t ss = 0;
        std::string sid;
        int n = 0;
        std::size_t lines = 0;
        std::size_t pos = 0;
    };

    static fs::path posPathOf(const fs::path& seg) { fs::path p = seg; p.replace_extension(".pos"); return p; }

    std::vector<Seg> listSegments() {
        std::vector<Seg> out;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = utf8Of(it->path().filename());
            if (name.rfind("journal-", 0) != 0 || it->path().extension() != ".jsonl") { continue; }
            Seg s;
            s.path = it->path();
            const std::string text = readAll(s.path);
            const std::size_t nl = text.find('\n');
            const json hdr = json::parse(text.substr(0, nl), nullptr, false);
            if (hdr.is_object()) { s.ss = hdr.value("ss", 0LL); }
            // journal-<sid>.<n>.jsonl
            const std::string stem = name.substr(0, name.size() - 6);
            const std::size_t dot = stem.rfind('.');
            if (dot != std::string::npos) {
                s.n = std::atoi(stem.c_str() + dot + 1);
                s.sid = stem.substr(8, dot - 8);
            }
            std::size_t lines = 0;
            for (std::size_t i = nl == std::string::npos ? text.size() : nl + 1; i < text.size(); ++i) { if (text[i] == '\n') { ++lines; } }
            s.lines = lines;
            const std::string pos = readAll(posPathOf(s.path));
            s.pos = pos.empty() ? 0 : static_cast<std::size_t>(std::strtoull(pos.c_str(), nullptr, 10));
            out.push_back(std::move(s));
        }
        std::sort(out.begin(), out.end(), [](const Seg& a, const Seg& b) {
            if (a.ss != b.ss) { return a.ss < b.ss; }
            if (a.sid != b.sid) { return a.sid < b.sid; }
            return a.n < b.n;
        });
        return out;
    }

    std::vector<fs::path> listFailFiles() {
        std::vector<fs::path> out;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = utf8Of(it->path().filename());
            if (name.rfind("fail-", 0) == 0 && it->path().extension() == ".json") { out.push_back(it->path()); }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    void countDropped(const std::string& key) {
        if (!key.empty()) { ++dropped[key]; }
        ++st.overflowDropped;
    }

    void dropSegment(const Seg& s) {
        const std::string text = readAll(s.path);
        std::size_t nl = text.find('\n');
        std::size_t idx = 0;
        while (nl != std::string::npos && nl + 1 < text.size()) {
            const std::size_t next = text.find('\n', nl + 1);
            if (next == std::string::npos) { break; }
            if (idx >= s.pos) {
                const json m = json::parse(text.substr(nl + 1, next - nl - 1), nullptr, false);
                countDropped(m.is_object() ? m.value("radioKey", std::string()) : std::string());
            }
            ++idx;
            nl = next;
        }
        std::error_code ec;
        fs::remove(s.path, ec);
        fs::remove(posPathOf(s.path), ec);
    }

    void dropFailFile(const fs::path& f) {
        const json w = json::parse(readAll(f), nullptr, false);
        std::string key;
        if (w.is_object() && w.contains("msg") && w["msg"].is_object()) { key = w["msg"].value("radioKey", std::string()); }
        countDropped(key);
        std::error_code ec;
        fs::remove(f, ec);
    }

    std::string currentSegmentName() const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "journal-%s.%d.jsonl", sessionId.c_str(), segNo);
        return buf;
    }

    // The folder's three bounds (contract 3.9): 200 failure files, 4 MiB, 14 days. The OLDEST is dropped and counted.
    void sweepBounds() {
        const std::int64_t t = now();
        std::vector<fs::path> fails = listFailFiles();
        for (const fs::path& f : fails) {
            const json w = json::parse(readAll(f), nullptr, false);
            const std::int64_t q = w.is_object() ? w.value("q", 0LL) : 0;
            if (q > 0 && t - q > o.maxAgeSec) { ++st.expired; dropFailFile(f); }
        }
        fails = listFailFiles();
        while (fails.size() > o.maxFailFiles) { dropFailFile(fails.front()); fails.erase(fails.begin()); }
        std::vector<Seg> segs = listSegments();
        const std::string active = currentSegmentName();
        for (const Seg& s : segs) {
            if (utf8Of(s.path.filename()) == active) { continue; }
            if (s.pos >= s.lines) {   // fully taken
                std::error_code ec;
                fs::remove(s.path, ec);
                fs::remove(posPathOf(s.path), ec);
                continue;
            }
            if (s.ss > 0 && t - s.ss > o.maxAgeSec + 1) { st.expired += s.lines - s.pos; dropSegment(s); }
        }
        segs = listSegments();
        auto folderBytes = [this]() {
            std::uintmax_t total = 0;
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                const auto sz = it->file_size(e2);
                if (!e2) { total += sz; }
            }
            return total;
        };
        while (folderBytes() > o.maxFolderBytes) {
            bool dropped1 = false;
            for (const Seg& s : segs) {
                if (utf8Of(s.path.filename()) == active) { continue; }
                dropSegment(s);
                dropped1 = true;
                break;
            }
            if (!dropped1) {
                std::vector<fs::path> f2 = listFailFiles();
                if (f2.empty()) { break; }
                dropFailFile(f2.front());
            }
            segs = listSegments();
        }
    }

    // ------------------------------------------------------------------ writing
    bool writeFailFile(const json& env, std::int64_t atEpoch) {
        json w;
        w["q"] = atEpoch;
        w["deg"] = 0;
        w["msg"] = env;
        char name[96];
        std::snprintf(name, sizeof(name), "fail-%012lld-%s.json", static_cast<long long>(atEpoch), randomHex(8).c_str());
        return writeAtomic(dir / name, dumpJson(w));
    }

    bool appendJournal(const json& env) {
        if (!segReady) {
            segNo = 0;
            segBytes = 0;
            segReady = true;
        }
        if (segBytes >= o.segmentBytes) { ++segNo; segBytes = 0; sweepBounds(); }
        const fs::path seg = dir / currentSegmentName();
        std::string data;
        std::error_code ec;
        if (!fs::exists(seg, ec)) {
            json h;
            h["hdr"] = 1;
            h["ss"] = sessionStart;
            data = dumpJson(h) + "\n";
        }
        data += dumpJson(env) + "\n";
        const bool ok = appendDurable(seg, data);
        if (ok) { segBytes += data.size(); }
        return ok;
    }

    // Takes a prepared message into the queue (the daily bound has already said yes).
    void admit(Pending p, bool isFail) {
        const bool anonymous = isFail && id.installId.empty();
        if (!isFail && id.installId.empty()) { return; }   // no anonymous journal
        // what the bound kept off the wire is said on THIS event of the same radio
        if (!p.key.empty()) {
            auto it = dropped.find(p.key);
            if (it != dropped.end() && it->second > 0) {
                p.body["dropped"] = std::min<long long>(it->second, 1000000);
                it->second = 0;
            }
        }
        json env = envelopeFor(p, anonymous);
        const std::string text = dumpJson(env);
        bool ok = false;
        if (isFail) {
            ok = writeFailFile(env, p.atEpoch);
            if (ok) { ++st.failuresQueued; }
        } else {
            ok = appendJournal(env);
            if (ok) { ++st.journalQueued; }
        }
        (void)text;
        if (!ok) { ++st.guardRefused; }
        if (++appendsSinceSweep >= 64 || isFail) { appendsSinceSweep = 0; sweepBounds(); }
    }

    void releaseHeld() {
        if (held.empty()) { return; }
        std::map<std::string, Pending> h;
        h.swap(held);
        for (auto& kv : h) {
            const bool fail = kv.second.isFail;
            admit(std::move(kv.second), fail);
        }
    }

    void rollover() {
        const std::string today = dayOf(now());
        if (day == today) { return; }
        // the day rolled over: what the bound held is sent now, then the counts start again
        releaseHeld();
        day = today;
        failAdmitted = 0;
        journalAdmitted = 0;
        firstSeen.clear();
        streamToday.clear();
    }

    // The daily bound for one message (contract 2.3 and 3.9). `tripleKey` is "key|class" for failures and "key|kind|word" for events.
    void boundedAdmit(Pending p, const std::string& tripleKey, bool isFail) {
        rollover();
        const int limit = isFail ? o.failuresPerDay : o.journalPerDay;
        int& counter = isFail ? failAdmitted : journalAdmitted;
        const bool first = firstSeen.insert(tripleKey).second;
        if (counter < limit || first) {
            ++counter;
            admit(std::move(p), isFail);
        } else {
            auto it = held.find(tripleKey);
            if (it != held.end()) {
                // an earlier held one is replaced: it is the one not sent, and it is said
                if (!p.key.empty()) { ++dropped[p.key]; }
                ++st.boundDropped;
            } else {
                ++st.boundHeld;
            }
            held[tripleKey] = std::move(p);
        }
        saveState();
    }

    // ------------------------------------------------------------------ sending
    bool queueEmpty() {
        if (!listFailFiles().empty()) { return false; }
        for (const Seg& s : listSegments()) { if (s.pos < s.lines) { return false; } }
        return true;
    }

    std::size_t limitFor(const std::string& kind) const {
        if (kind == "radio_fail") { return kMaxFailBytes; }
        if (kind == "first_session") { return kMaxFirstSessionBytes; }
        return kMaxJournalBytes;
    }

    // Shrinks a message that is over its kind's size (contract: 16,384 / 8,192 / 4,096): the log lines first, then the reason.
    std::string fitSize(json env) {
        const std::string kind = env.value("kind", std::string());
        const std::size_t cap = limitFor(kind);
        std::string text = dumpJson(env);
        while (text.size() > cap && env.contains("body") && env["body"].contains("logLines") && !env["body"]["logLines"].empty()) {
            json& ll = env["body"]["logLines"];
            ll.erase(ll.begin(), ll.begin() + static_cast<std::ptrdiff_t>(std::max<std::size_t>(1, ll.size() / 2)));
            text = dumpJson(env);
        }
        if (text.size() > cap && env.contains("body") && env["body"].contains("reason")) {
            env["body"]["reason"] = clipUtf8(env["body"]["reason"].get<std::string>(), 120);
            text = dumpJson(env);
        }
        return text;
    }

    // The same message with its free text taken out (the resend of contract 2.2).
    static json degrade(json env) {
        json& b = env["body"];
        const std::string kind = env.value("kind", std::string());
        if (kind == "radio_fail") {
            const std::string code = "withheld";
            b["reason"] = code;
            b["logLines"] = json::array();
        } else {
            for (const char* f : {"reason", "usbHost", "antenna", "gpu", "usbHosts"}) { b.erase(f); }
            if (b.contains("cpu")) { b["cpu"] = "other"; }
        }
        return env;
    }

    std::vector<Item> collect(std::int64_t t, int maxItems) {
        std::vector<Item> items;
        // failures first (they are urgent and few), oldest first
        for (const fs::path& f : listFailFiles()) {
            if (static_cast<int>(items.size()) >= maxItems) { return items; }
            const json w = json::parse(readAll(f), nullptr, false);
            if (!w.is_object() || !w.contains("msg") || !w["msg"].is_object()) {
                std::error_code ec;
                fs::remove(f, ec);   // an unreadable file is not a report
                continue;
            }
            Item it;
            it.isFail = true;
            it.file = f;
            it.kind = "radio_fail";
            it.env = w["msg"];
            it.degraded = w.value("deg", 0) != 0;
            it.anonymous = !it.env.contains("installId");
            it.radioKey = it.env.value("radioKey", std::string());
            it.eventEpoch = w.value("q", t);
            items.push_back(std::move(it));
        }
        for (const Seg& s : listSegments()) {
            if (s.pos >= s.lines) { continue; }
            const std::string text = readAll(s.path);
            std::size_t nl = text.find('\n');
            std::size_t idx = 0;
            while (nl != std::string::npos && nl + 1 < text.size()) {
                const std::size_t next = text.find('\n', nl + 1);
                if (next == std::string::npos) { break; }
                if (idx >= s.pos) {
                    if (static_cast<int>(items.size()) >= maxItems) { return items; }
                    Item it;
                    it.file = s.path;
                    it.line = idx;
                    it.env = json::parse(text.substr(nl + 1, next - nl - 1), nullptr, false);
                    if (it.env.is_object()) {
                        it.kind = it.env.value("kind", std::string());
                        it.radioKey = it.env.value("radioKey", std::string());
                        it.eventEpoch = s.ss + it.env.value("t", 0LL);
                    }
                    items.push_back(std::move(it));
                }
                ++idx;
                nl = next;
            }
        }
        return items;
    }

    void advance(const Item& it) {
        if (it.isFail) {
            std::error_code ec;
            fs::remove(it.file, ec);
            return;
        }
        writeAtomic(posPathOf(it.file), std::to_string(it.line + 1));
    }

    void backoff(std::int64_t t) {
        static const std::int64_t kSteps[] = {60, 300, 3600, 21600};
        const int i = std::min(failStreak, 3);
        ++failStreak;
        backoffUntil = t + kSteps[i];
    }

    int runBatch() {
        std::lock_guard<std::mutex> batchLock(batchMu);
        if (!on) { return 0; }
        if (!o.liveSend) {
            std::lock_guard<std::mutex> lk(mu);
            ++st.suppressedByBuild;
            return 0;
        }
        int accepted = 0;
        std::vector<Item> items;
        std::int64_t t = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            t = now();
            if (t < backoffUntil) { return 0; }
            items = collect(t, o.maxBatch);
        }
        const Transport transport = o.transport ? o.transport : makeRealTransport(cancel);
        for (Item& it : items) {
            if (cancel->cancelled()) { break; }
            std::int64_t sendNow = 0;
            {
                std::lock_guard<std::mutex> lk(mu);
                sendNow = now();
                if (!it.env.is_object()) { advance(it); continue; }
                if (sendNow - it.eventEpoch > o.maxAgeSec) { ++st.expired; advance(it); continue; }
            }
            bool resent = false;
            for (;;) {
                json env = it.env;
                if (it.degraded) { env = degrade(env); }
                env["age"] = std::clamp<std::int64_t>(sendNow - it.eventEpoch, 0, kMaxT);
                const std::string text = fitSize(env);
                const PostResult res = transport(o.endpoint, text);
                std::lock_guard<std::mutex> lk(mu);
                ++st.posts;
                const std::int64_t after = now();
                if (!res.attempted || res.status == 0 || res.status >= 500) { backoff(after); return accepted; }
                if (res.status >= 200 && res.status < 300) {
                    ++st.sent;
                    ++accepted;
                    failStreak = 0;
                    advance(it);
                    break;
                }
                if (res.status == 429) { backoffUntil = after + static_cast<std::int64_t>(std::max<std::uint64_t>(res.retryAfterSec, 1)); return accepted; }
                std::string body = res.body;
                while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) { body.pop_back(); }
                if (res.status == 400 && body == "bad id") {
                    // the Worker deployed today: not ready, so keep it and try again later
                    ++st.notReady;
                    backoff(after);
                    return accepted;
                }
                std::string code;
                if (body.rfind("refused: ", 0) == 0) { code = body.substr(9); }
                if ((res.status == 400 || res.status == 413) && !code.empty()) {
                    if (textRefusalCode(code) && !it.degraded && !resent) {
                        // send it once more with the text withheld: the failure itself must still reach the owner
                        it.degraded = true;
                        resent = true;
                        ++st.degradedResends;
                        if (it.isFail) {
                            json w = json::parse(readAll(it.file), nullptr, false);
                            if (w.is_object()) { w["deg"] = 1; writeAtomic(it.file, dumpJson(w)); }
                        }
                        continue;
                    }
                    ++st.refused;
                    ++refusedTotal;
                    advance(it);
                    break;
                }
                // any other answer (a 400 that is not ours, a 403, a 404): the route is wrong or the service is not what we think; keep and back off
                backoff(after);
                return accepted;
            }
        }
        return accepted;
    }

    void fileFailureLocked(const FailureInput& in, const std::vector<std::string>& logRing, std::int64_t t) {
    if (!on) { return; }
        const RadioIdentity rid = in.soundCard ? soundCardIdentity() : identify(in.driverKind, in.args, in.nativeLabel);
            Impl::Pending p;
        p.kind = "radio_fail";
        p.key = rid.key;
        p.isFail = true;
        p.atEpoch = t;
        p.t = t - sessionStart;
        p.cls = in.reasonClass.empty() ? std::string("other") : in.reasonClass;
        json b;
        b["driver"] = rid.driver;
        b["model"] = rid.model;
        b["serial"] = rid.serial;
        b["reason"] = scrubReason(in.reason, rid.serial, 400);
        b["reasonClass"] = p.cls;
        b["trigger"] = triggerWord(in.trigger);
        b["attempt"] = static_cast<long long>(std::clamp<long long>(++failAttempt[rid.key], 1, 10000));
        if (in.openMs >= 0) { b["openMs"] = std::min<long long>(in.openMs, 3600000); }
        if (in.sampleRateSps >= 0) { b["sampleRateSps"] = std::min<long long>(in.sampleRateSps, 1000000000); }
        b["superseded"] = in.superseded;
        b["diagOn"] = id.diagOn;
        b["logLines"] = radioLines(logRing, rid.serial, 40);
        b["v"] = clipUtf8(id.version.empty() ? std::string("unknown") : id.version, 48);
        if (!id.channel.empty()) { b["ch"] = id.channel; }
        if (id.updateChannel == "stable" || id.updateChannel == "beta") { b["uch"] = id.updateChannel; }
        b["os"] = clipUtf8(id.os.empty() ? std::string("unknown") : id.os, 40);
        b["arch"] = clipUtf8(id.arch.empty() ? std::string("unknown") : id.arch, 8);
        p.body = std::move(b);
        if (hasForbiddenKey(p.body) || anyStringForbidden(p.body)) { ++st.guardRefused; return; }
        urgent = true;
        boundedAdmit(std::move(p), rid.key + "|" + (in.reasonClass.empty() ? std::string("other") : in.reasonClass), /*isFail=*/true);
        nextBatchAt = 0;   // a failure is sent at once
    }

    void eventLocked(const Event& ev, std::int64_t t) {
    if (!on || id.installId.empty()) { return; }
        if (!inList(kindsOfJournal(), ev.kind)) { ++st.guardRefused; return; }
        const bool radioKind = ev.kind != "session_env" && ev.kind != "session_end";
        if (ev.kind == "session_env") {
            if (envSent) { return; }
            envSent = true;
        }
            Impl::Pending p;
        p.kind = ev.kind;
        p.key = radioKind ? ev.radio.key : std::string();
        p.atEpoch = t;
        p.t = ev.atSec >= 0 ? ev.atSec : t - sessionStart;
        json b = json::object();
        if (radioKind) {
            b["driver"] = ev.radio.driver;
            b["model"] = ev.radio.model;
            b["serial"] = ev.radio.serial;
        }
        b["v"] = clipUtf8(id.version.empty() ? std::string("unknown") : id.version, 48);
        if (ev.kind == "session_env") {
            b["os"] = clipUtf8(id.os.empty() ? std::string("unknown") : id.os, 40);
            b["arch"] = clipUtf8(id.arch.empty() ? std::string("unknown") : id.arch, 8);
            if (!id.channel.empty()) { b["ch"] = id.channel; }
            if (id.updateChannel == "stable" || id.updateChannel == "beta") { b["uch"] = id.updateChannel; }
            if (id.window == "new" || id.window == "legacy") { b["window"] = id.window; }
            b["launches"] = static_cast<long long>(std::clamp<std::uint64_t>(id.launches, 1, 1000000));
            b["diagOn"] = id.diagOn;
        }
        std::string word = ev.word;
        for (const auto& f : ev.fields) {
            const std::string& name = f.first;
            if (forbiddenKey(name)) { ++st.guardRefused; return; }
            json v;
            if (const auto* x = std::get_if<std::string>(&f.second)) { v = *x; if (name == "note") { word = *x; } }
            else if (const auto* l = std::get_if<long long>(&f.second)) { v = *l; }
            else if (const auto* d = std::get_if<double>(&f.second)) { v = *d; }
            else if (const auto* bo = std::get_if<bool>(&f.second)) { v = *bo; }
            else if (const auto* vs = std::get_if<std::vector<std::string>>(&f.second)) { v = *vs; }
            else if (const auto* vl = std::get_if<std::vector<long long>>(&f.second)) { v = *vl; }
            // a text field is scrubbed whatever the caller did, and a value that still fails is left out
            if (textFieldNames().count(name) != 0 && name != "model") {
                if (v.is_string()) {
                    v = scrubText(v.get<std::string>(), ev.radio.serial, 400);
                } else if (v.is_array()) {
                    json kept = json::array();
                    for (const json& x : v) { if (x.is_string()) { kept.push_back(scrubText(x.get<std::string>(), ev.radio.serial, 64)); } }
                    v = std::move(kept);
                }
            }
            b[name] = std::move(v);
        }
        if (ev.kind == "radio_stream") {
            // at most one per radio per minute (five after the day's 500th): a faster caller is coarsened, not sent
            const bool note = b.contains("note") && b["note"].is_string() && !b["note"].get<std::string>().empty();
            auto last = lastStreamAt.find(p.key);
            const int window = (streamToday[p.key] >= o.streamWidenAfter) ? 300 : 60;
            if (!note && last != lastStreamAt.end() && t - last->second < window - 1) { return; }
            lastStreamAt[p.key] = t;
            ++streamToday[p.key];
        }
        if (ev.kind == "radio_open" && ev.word == "start") {
            b["attempt"] = static_cast<long long>(std::clamp<long long>(++openAttempt[p.key], 1, 10000));
        } else if (ev.kind == "radio_open") {
            b["attempt"] = static_cast<long long>(std::clamp<long long>(openAttempt[p.key], 1, 10000));
        }
        if (ev.kind == "radio_recovery" && ev.word == "start") {
            b["attempt"] = static_cast<long long>(std::clamp<long long>(++recoveryAttempt[p.key], 1, 10000));
        } else if (ev.kind == "radio_recovery") {
            b["attempt"] = static_cast<long long>(std::clamp<long long>(recoveryAttempt[p.key], 1, 10000));
        }
        // `band` is a NAME from the contract's list, never a number: a value that is not one of them is a frequency in disguise and refuses the event
        if (b.contains("band")) {
            static const std::vector<std::string> kBands = {"lf", "mf", "hf", "vhf-low", "fm-broadcast", "air-band", "vhf-high", "uhf", "l-band", "shf", "other"};
            if (!b["band"].is_string() || !inList(kBands, b["band"].get<std::string>())) { ++st.guardRefused; return; }
        }
        p.body = std::move(b);
        if (hasForbiddenKey(p.body) || anyStringForbidden(p.body)) { ++st.guardRefused; return; }
        for (const std::string& name : textFieldNames()) {
            if (!p.body.contains(name) || !p.body[name].is_string()) { continue; }
            if (!textCode(p.body[name].get<std::string>(), ev.radio.serial).empty()) { ++st.guardRefused; return; }
        }
        if (ev.urgent) { urgent = true; nextBatchAt = 0; }
        if (ev.kind == "session_env") {
            // once a session and never bounded
            rollover();
            admit(std::move(p), false);
            saveState();
            return;
        }
        boundedAdmit(std::move(p), fieldKey(ev.radio.key, ev.kind, word), /*isFail=*/false);
    }

    void reapWorker() {
        if (worker.joinable() && !running.load()) { worker.join(); }
    }

    void startBatchThread(const std::shared_ptr<Impl>& self) {
        reapWorker();
        if (worker.joinable()) { return; }
        running.store(true);
        worker = std::thread([self]() {
            self->runBatch();
            self->running.store(false);
        });
    }
};

// ---------------------------------------------------------------------------

Reporter::Reporter() : p_(std::make_shared<Impl>()) {}
Reporter::~Reporter() { shutdown(); }

void Reporter::configure(const Options& options) {
    shutdown();
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    s.o = options;
    s.id = options.identity;
    s.on = !options.dir.empty() && !options.endpoint.empty();
    s.dir = pathOf(options.dir);
    s.cancel = std::make_shared<UploadCancel>();
    s.sessionId = options.sessionId.empty() ? randomHex(16) : options.sessionId;
    s.sessionStart = options.sessionStartEpoch > 0 ? options.sessionStartEpoch : s.now();
    for (const Impl::PreFail& f : s.preFails) { s.sessionStart = std::min(s.sessionStart, f.at); }
    for (const Impl::PreEvent& e : s.preEvents) { s.sessionStart = std::min(s.sessionStart, e.at); }
    s.configuredOnce = true;
    s.seq = 0;
    s.held.clear();
    s.firstSeen.clear();
    s.dropped.clear();
    s.streamToday.clear();
    s.failAttempt.clear();
    s.openAttempt.clear();
    s.recoveryAttempt.clear();
    s.lastStreamAt.clear();
    s.segNo = 0;
    s.segBytes = 0;
    s.segReady = false;
    s.envSent = false;
    s.firstSessionSent = 0;
    s.firstSessionLastAt = -1;
    s.nextBatchAt = 0;
    s.backoffUntil = 0;
    s.failStreak = 0;
    s.day.clear();
    s.failAdmitted = 0;
    s.journalAdmitted = 0;
    if (!s.on) { s.preFails.clear(); s.preEvents.clear(); return; }
    std::error_code ec;
    fs::create_directories(s.dir, ec);
    if (ec) { s.on = false; s.preFails.clear(); s.preEvents.clear(); return; }
    s.loadState();
    // what an earlier day held is released at THIS start; the day's counts of today start from what the file says
    s.rollover();
    if (!s.held.empty() && s.day == dayOf(s.now())) {
        // held from earlier in the same day by a run that ended without the quit flush (a crash): it is sent now, it is the last of its loop
        s.releaseHeld();
    }
    s.sweepBounds();
    s.saveState();
    // WHAT HAPPENED BEFORE THE QUEUE COULD BE OPENED (the start-up restore runs before Usage reporting is read): filed now, with the time it happened
    {
        std::vector<Impl::PreFail> pf;
        pf.swap(s.preFails);
        std::vector<Impl::PreEvent> pe;
        pe.swap(s.preEvents);
        for (const Impl::PreFail& f : pf) { s.fileFailureLocked(f.in, f.ring, f.at); }
        for (const Impl::PreEvent& e : pe) { s.eventLocked(e.ev, e.at); }
    }
}

bool Reporter::active() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->on; }

void Reporter::setIdentity(const Identity& idn) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    const bool hadId = !s.id.installId.empty();
    s.id = idn;
    if (!s.on) { return; }
    if (hadId && idn.installId.empty()) {
        // Usage reporting went OFF: the install id is deleted, so everything that carries it goes with it. A queued failure is kept, as the
        // anonymous form (the owner's rule is that every failure reaches him); the journal and first-session lines are deleted.
        std::error_code ec;
        for (const Impl::Seg& g : s.listSegments()) { fs::remove(g.path, ec); fs::remove(Impl::posPathOf(g.path), ec); }
        s.segReady = false;
        for (const fs::path& f : s.listFailFiles()) {
            json w = json::parse(readAll(f), nullptr, false);
            if (!w.is_object() || !w.contains("msg") || !w["msg"].is_object()) { continue; }
            json& m = w["msg"];
            if (!m.contains("installId")) { continue; }
            m.erase("installId");
            m.erase("sessionId");
            m.erase("seq");
            m["messageId"] = randomHex(16);
            writeAtomic(f, dumpJson(w));
        }
        s.envSent = false;
        s.firstSessionSent = 0;
    }
    if (!hadId && !idn.installId.empty()) { s.envSent = false; }
}

void Reporter::setLiveSend(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->o.liveSend = on && kRadioReportsLiveSend;
}

void Reporter::setNowSource(std::function<std::int64_t()> now) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->o.nowEpoch = std::move(now);
}

void Reporter::setDiagOn(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->id.diagOn = on;
}

Identity Reporter::identity() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->id; }

void Reporter::fileFailure(const FailureInput& in, const std::vector<std::string>& logRing) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    if (!s.configuredOnce) {   // before the queue is opened (the start-up restore runs first): kept in memory and filed at configure
        if (s.preFails.size() < 32) { s.preFails.push_back(Impl::PreFail{in, logRing, s.now()}); }
        return;
    }
    s.fileFailureLocked(in, logRing, s.now());
}

void Reporter::event(const Event& ev) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    if (!s.configuredOnce) {
        if (s.preEvents.size() < 64) { s.preEvents.push_back(Impl::PreEvent{ev, s.now()}); }
        return;
    }
    s.eventLocked(ev, s.now());
}

void Reporter::firstSession(const std::string& usageJson, const std::string& trigger) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    if (!s.on || s.id.installId.empty() || s.id.launches != 1) { return; }
    if (trigger != "after-minute" && trigger != "on-failure") { return; }
    if (s.firstSessionSent >= 6) { return; }
    const std::int64_t t = s.now();
    if (s.firstSessionLastAt >= 0 && t - s.firstSessionLastAt < 60) { return; }
    const json u = json::parse(usageJson, nullptr, false);
    if (!u.is_object()) { return; }
    static const char* const kFields[] = {"v", "os", "arch", "launches", "crashes", "sessionSec", "sdr", "modes", "panels", "plugins", "ch",
                                          "first", "fv", "uch", "stalls", "health", "exits_died", "exits_killed", "exits_ended", "exits_unknown"};
    json b = json::object();
    for (const char* f : kFields) { if (u.contains(f)) { b[f] = u[f]; } }
    b["launches"] = 1;
    b["trigger"] = trigger;
    if (b.contains("sdr") && b["sdr"].is_string()) { b["sdr"] = scrubText(b["sdr"].get<std::string>(), std::string(), 64); }
    if (hasForbiddenKey(b) || anyStringForbidden(b)) { ++s.st.guardRefused; return; }
    Impl::Pending p;
    p.kind = "first_session";
    p.atEpoch = t;
    p.t = t - s.sessionStart;
    p.body = std::move(b);
    ++s.firstSessionSent;
    s.firstSessionLastAt = t;
    s.rollover();
    s.admit(std::move(p), false);
    if (trigger == "on-failure") { s.urgent = true; s.nextBatchAt = 0; }
    s.saveState();
}

void Reporter::setOpenRadios(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->openKeys = keys;
    if (p_->openKeys.size() > 8) { p_->openKeys.resize(8); }
}

std::vector<std::string> Reporter::openRadios() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->openKeys; }

int Reporter::streamWindowSec(const std::string& radioKey) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->streamToday.find(radioKey);
    return (it != p_->streamToday.end() && it->second >= p_->o.streamWidenAfter) ? 300 : 60;
}

void Reporter::poll() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.on) { return; }
    s.reapWorker();
    const std::int64_t t = s.now();
    if (s.running.load() || s.worker.joinable()) { return; }
    if (t < s.nextBatchAt && !s.urgent) { return; }
    if (s.queueEmpty()) { s.nextBatchAt = t + s.o.batchEverySec; s.urgent = false; return; }
    s.nextBatchAt = t + s.o.batchEverySec;
    s.urgent = false;
    if (!s.o.liveSend) { ++s.st.suppressedByBuild; return; }
    s.startBatchThread(p_);
}

int Reporter::runBatchNow() { return p_->runBatch(); }

void Reporter::flushAtQuit(int maxMs) {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (!p_->on) { return; }
        p_->releaseHeld();
        p_->saveState();
        p_->reapWorker();
        if (!p_->o.liveSend) { return; }
        if (!p_->queueEmpty()) { p_->startBatchThread(p_); }
    }
    // A bounded wait for the last send: quit is never delayed by a stuck one. The transfer is abortable, so the cancel ends it at once; a thread that
    // still has not returned is left to finish on its own (it owns what it uses).
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, maxMs));
    while (p_->running.load() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    if (p_->running.load()) {
        p_->cancel->cancel();
        const auto grace = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (p_->running.load() && std::chrono::steady_clock::now() < grace) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    if (p_->worker.joinable()) {
        if (p_->running.load()) { p_->worker.detach(); } else { p_->worker.join(); }
    }
}

void Reporter::shutdown() {
    Impl& s = *p_;
    if (s.worker.joinable()) {
        s.cancel->cancel();
        s.worker.join();
    }
}

Stats Reporter::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Stats st = p_->st;
    if (p_->on) {
        st.failFilesOnDisk = p_->listFailFiles().size();
        std::size_t pending = 0;
        for (const Impl::Seg& g : p_->listSegments()) { pending += g.lines > g.pos ? g.lines - g.pos : 0; }
        st.journalLinesPending = pending;
    }
    return st;
}

std::string Reporter::sessionId() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->sessionId; }
std::int64_t Reporter::nowEpoch() const { return p_->now(); }
std::string Reporter::dir() const { std::lock_guard<std::mutex> lk(p_->mu); return utf8Of(p_->dir); }
std::size_t Reporter::heldCount() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->held.size(); }

Reporter& global() {
    static Reporter r;
    return r;
}

void noteFound(const std::string& driverKind, const std::string& args, const std::string& label, const std::string& binding, const std::string& source,
               const std::string& usbId) {
    Event ev;
    ev.kind = "radio_found";
    ev.radio = identify(driverKind, args, label);
    ev.word = binding;
    ev.set("binding", binding).set("source", source);
    if (!usbId.empty()) { ev.set("usbId", usbId); }
    global().event(ev);
}

void noteRecovery(const std::string& driverKind, const std::string& args, const char* phase, const std::string& what, const std::string& reason, long long ms) {
    Event ev;
    ev.kind = "radio_recovery";
    ev.radio = driverKind == "soundcard" ? soundCardIdentity() : identify(driverKind, args);
    const std::string ph = phase != nullptr ? phase : "start";
    ev.word = ph;
    ev.set("phase", ph).set("what", what);
    if (!reason.empty()) {
        ev.set("reasonClass", health::radioReasonWord(health::classifyRadioOpen(driverKind, reason)));
        ev.text("reason", reason, 400);
    }
    if (ms >= 0) { ev.set("ms", ms); }
    ev.urgent = ph == "gave-up";
    global().event(ev);
}

std::vector<std::string> readQueueFiles(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(pathOf(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) { out.push_back(readAll(it->path())); }
    }
    return out;
}

}  // namespace cascade::core::radioreport
