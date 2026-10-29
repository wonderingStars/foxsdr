// See plugin_marker.hpp for the contract.
//
// THE MARKER'S MEMORY MODEL. set() and clear() take a spin lock around the slot table (two threads creating two plugins' instances at once
// is the normal case); blame() takes NONE and is plain stores into the mapped bytes, because it runs from the crash handler, where a lock
// another thread holds is a deadlock. A blame racing a set() on the same slot can lose to it; the next start then sees the phase and not
// the blame, which is the same evidence by another route (the slot was in play) - never a wrong accusation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_marker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

#include "core/diag_log.hpp"
#include "core/leak_on_purpose.hpp"
#include "core/plugin_run.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cascade::core {

namespace marker {

namespace {

constexpr std::uint32_t kMagic = 0x4D505846u;  // "FXPM" little-endian
constexpr std::uint32_t kLayout = 1;
constexpr std::size_t kIdBytes = 48;
constexpr std::size_t kVersionBytes = 24;

struct Header {
    std::uint32_t magic;
    std::uint32_t layout;
    std::uint32_t pid;
    std::uint32_t reserved;
    std::int64_t startUnix;
    std::uint8_t pad[40];
};
struct SlotRec {
    char id[kIdBytes];
    char version[kVersionBytes];
    std::uint8_t phase;
    std::uint8_t blame;
    std::uint8_t origin;
    std::uint8_t used;
    std::uint32_t spare;
};
static_assert(sizeof(Header) == 64, "the marker header is 64 bytes");
static_assert(sizeof(SlotRec) == 80, "a marker slot is 80 bytes");
static_assert(sizeof(Header) + kSlots * sizeof(SlotRec) <= kFileBytes, "the slots fit the file");

// Never destroyed: a worker that sets a phase after main() has returned finds the table still there.
struct State {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::uint8_t memory[kFileBytes] = {};
    std::uint8_t* base = nullptr;  // the mapped view, or `memory`
    bool mapped = false;
    std::atomic<std::size_t> dropped{0};
    std::mutex ranMutex;
    std::set<std::string> ran;
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int fd = -1;
#endif
};

// Set when a marker is open: the CRASH path (blame) asks this flag, not state(), so a first use never allocates inside a fault handler.
std::atomic<State*> g_open{nullptr};

State& state() {
    static State* const s = cascade::core::leakOnPurpose(new State());
    return *s;
}

struct SpinLock {
    explicit SpinLock(std::atomic_flag& f) : f_(f) {
        while (f_.test_and_set(std::memory_order_acquire)) {}
    }
    ~SpinLock() { f_.clear(std::memory_order_release); }
    std::atomic_flag& f_;
};

std::uint32_t steadyMs32() noexcept {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

SlotRec* slots(State& st) { return reinterpret_cast<SlotRec*>(st.base + sizeof(Header)); }

std::string normalise(const std::string& key) {
    std::string id = pluginRunId(key);
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    if (id.size() >= kIdBytes) { id.resize(kIdBytes - 1); }
    return id;
}

void unmap(State& st) {
#if defined(_WIN32)
    if (st.mapped && st.base != nullptr) { ::UnmapViewOfFile(st.base); }
    if (st.mapping != nullptr) { ::CloseHandle(st.mapping); }
    if (st.file != INVALID_HANDLE_VALUE) { ::CloseHandle(st.file); }
    st.mapping = nullptr;
    st.file = INVALID_HANDLE_VALUE;
#else
    if (st.mapped && st.base != nullptr) { ::munmap(st.base, kFileBytes); }
    if (st.fd >= 0) { ::close(st.fd); }
    st.fd = -1;
#endif
    st.mapped = false;
    st.base = nullptr;
}

void parsePrevious(const std::uint8_t* bytes, std::size_t n, std::vector<InPlay>& out) {
    if (n < sizeof(Header) + kSlots * sizeof(SlotRec)) { return; }
    Header h;
    std::memcpy(&h, bytes, sizeof h);
    if (h.magic != kMagic || h.layout != kLayout) { return; }
    for (std::size_t i = 0; i < kSlots; ++i) {
        SlotRec r;
        std::memcpy(&r, bytes + sizeof(Header) + i * sizeof(SlotRec), sizeof r);
        if (r.used != 1) { continue; }
        r.id[kIdBytes - 1] = '\0';
        r.version[kVersionBytes - 1] = '\0';
        if (r.id[0] == '\0' || r.phase > static_cast<std::uint8_t>(Phase::Unload)) { continue; }
        InPlay p;
        p.id = r.id;
        p.version = r.version;
        p.origin = static_cast<fault::Origin>(r.origin <= 2 ? r.origin : 0);
        p.phase = static_cast<Phase>(r.phase);
        p.blamed = r.blame != 0;
        // A slot is in the file for a reason: in play (a phase) or accused (a blame). An idle, unblamed slot is a plugin that was out of
        // play and is not evidence of anything.
        if (p.phase == Phase::Idle && !p.blamed) { continue; }
        out.push_back(std::move(p));
    }
}

void stampHeader(State& st) {
    std::memset(st.base, 0, kFileBytes);
    Header h{};
    h.magic = kMagic;
    h.layout = kLayout;
#if defined(_WIN32)
    h.pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    h.pid = static_cast<std::uint32_t>(::getpid());
#endif
    h.startUnix = static_cast<std::int64_t>(std::time(nullptr));
    std::memcpy(st.base, &h, sizeof h);
}

}  // namespace

const char* phaseWord(Phase p) {
    switch (p) {
        case Phase::Load: return "load";
        case Phase::Create: return "create";
        case Phase::FirstPass: return "first-pass";
        case Phase::Running: return "running";
        case Phase::Unload: return "unload";
        case Phase::Idle: break;
    }
    return "idle";
}

bool parsePhase(const std::string& word, Phase& out) {
    for (const Phase p : {Phase::Idle, Phase::Load, Phase::Create, Phase::FirstPass, Phase::Running, Phase::Unload}) {
        if (word == phaseWord(p)) {
            out = p;
            return true;
        }
    }
    return false;
}

std::vector<InPlay> open(const std::string& path) {
    State& st = state();
    SpinLock lk(st.lock);
    unmap(st);
    std::vector<InPlay> previous;
    if (!path.empty()) {
        // What the last run left, read as a plain file BEFORE the mapping resets it.
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                parsePrevious(bytes.data(), bytes.size(), previous);
            }
        }
#if defined(_WIN32)
        const std::wstring wide = std::filesystem::path(path).wstring();
        HANDLE f = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            sz.QuadPart = static_cast<LONGLONG>(kFileBytes);
            if (::SetFilePointerEx(f, sz, nullptr, FILE_BEGIN) && ::SetEndOfFile(f)) {
                HANDLE m = ::CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kFileBytes), nullptr);
                if (m != nullptr) {
                    void* v = ::MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, kFileBytes);
                    if (v != nullptr) {
                        st.file = f;
                        st.mapping = m;
                        st.base = static_cast<std::uint8_t*>(v);
                        st.mapped = true;
                    } else {
                        ::CloseHandle(m);
                    }
                }
            }
            if (!st.mapped) { ::CloseHandle(f); }
        }
#else
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (::ftruncate(fd, static_cast<off_t>(kFileBytes)) == 0) {
                void* v = ::mmap(nullptr, kFileBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (v != MAP_FAILED) {
                    st.fd = fd;
                    st.base = static_cast<std::uint8_t*>(v);
                    st.mapped = true;
                }
            }
            if (!st.mapped) { ::close(fd); }
        }
#endif
        if (!st.mapped) { diagWarnf("plugins: the crash-loop marker could not be kept on disk; this run is not protected against a plugin that crashes at start-up"); }
    }
    if (!st.mapped) { st.base = st.memory; }
    stampHeader(st);
    g_open.store(&st, std::memory_order_release);
    {
        std::lock_guard<std::mutex> g(st.ranMutex);
        st.ran.clear();
    }
    return previous;
}

bool persistent() { return state().mapped; }

void set(const std::string& pluginKeyOrId, Phase phase, const std::string& version, fault::Origin origin) noexcept {
    State& st = state();
    if (st.base == nullptr) { return; }   // open() has not been called: nothing is kept
    std::string id;
    try {
        id = normalise(pluginKeyOrId);
    } catch (...) {
        return;
    }
    if (id.empty()) { return; }
    {
        SpinLock lk(st.lock);
        SlotRec* s = slots(st);
        SlotRec* at = nullptr;
        SlotRec* free = nullptr;
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (s[i].used == 1) {
                if (std::strncmp(s[i].id, id.c_str(), kIdBytes) == 0) { at = &s[i]; break; }
            } else if (free == nullptr) {
                free = &s[i];
            }
        }
        if (at == nullptr) { at = free; }
        if (at == nullptr) {
            st.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (at->used != 1) {
            std::memset(at, 0, sizeof *at);
            std::memcpy(at->id, id.c_str(), std::min(id.size(), kIdBytes - 1));
            at->used = 1;
        }
        if (!version.empty()) {
            std::memset(at->version, 0, kVersionBytes);
            std::memcpy(at->version, version.data(), std::min(version.size(), kVersionBytes - 1));
        }
        if (origin != fault::Origin::Unrecorded) { at->origin = static_cast<std::uint8_t>(origin); }
        at->phase = static_cast<std::uint8_t>(phase);
        // (the clock of first-pass; the low 32 bits of the steady clock in milliseconds, wrap-safe by unsigned subtraction)
        if (phase == Phase::FirstPass) { at->spare = steadyMs32(); }
    }
    if (phase == Phase::Running) {
        try {
            std::lock_guard<std::mutex> g(st.ranMutex);
            if (st.ran.size() < kSlots * 4) { st.ran.insert(id); }
        } catch (...) {
        }
    }
}

void clear(const std::string& pluginKeyOrId) noexcept {
    State& st = state();
    if (st.base == nullptr) { return; }
    std::string id;
    try {
        id = normalise(pluginKeyOrId);
    } catch (...) {
        return;
    }
    if (id.empty()) { return; }
    SpinLock lk(st.lock);
    SlotRec* s = slots(st);
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (s[i].used == 1 && std::strncmp(s[i].id, id.c_str(), kIdBytes) == 0) {
            std::memset(&s[i], 0, sizeof s[i]);
            return;
        }
    }
}

Phase phaseOf(const std::string& pluginKeyOrId) noexcept {
    State& st = state();
    if (st.base == nullptr) { return Phase::Idle; }
    std::string id;
    try {
        id = normalise(pluginKeyOrId);
    } catch (...) {
        return Phase::Idle;
    }
    if (id.empty()) { return Phase::Idle; }
    SpinLock lk(st.lock);
    const SlotRec* s = slots(st);
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (s[i].used == 1 && std::strncmp(s[i].id, id.c_str(), kIdBytes) == 0) { return static_cast<Phase>(s[i].phase); }
    }
    return Phase::Idle;
}

CallPhase::CallPhase(const std::string& pluginKeyOrId, Phase during, const std::string& version, fault::Origin origin) noexcept
    : before_(Phase::Idle) {
    try {
        key_ = pluginKeyOrId;
    } catch (...) {
        return;
    }
    before_ = phaseOf(key_);
    set(key_, during, version, origin);
}

CallPhase::~CallPhase() {
    if (key_.empty()) { return; }
    if (before_ == Phase::Idle) {
        // It had a slot at best as a loaded module; keep that slot (so a crash can still be blamed on it) and drop only the phase.
        set(key_, Phase::Idle);
    } else {
        set(key_, before_);
    }
}

void blame(const char* runId) noexcept {
    State* const open = g_open.load(std::memory_order_acquire);
    if (open == nullptr || open->base == nullptr || runId == nullptr || runId[0] == '\0') { return; }
    State& st = *open;
    SlotRec* s = slots(st);
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (s[i].used == 1 && std::strncmp(s[i].id, runId, kIdBytes) == 0) {
            s[i].blame = 1;
            return;
        }
    }
}

void cleanShutdown() noexcept {
    State& st = state();
    if (st.base == nullptr) { return; }
    SpinLock lk(st.lock);
    std::memset(st.base + sizeof(Header), 0, kSlots * sizeof(SlotRec));
}

void promoteFirstPass(std::uint32_t nowMs, std::uint32_t afterMs) noexcept {
    State& st = state();
    if (st.base == nullptr) { return; }
    std::vector<std::string> due;
    {
        SpinLock lk(st.lock);
        SlotRec* s = slots(st);
        for (std::size_t i = 0; i < kSlots; ++i) {
            if (s[i].used == 1 && s[i].phase == static_cast<std::uint8_t>(Phase::FirstPass) && static_cast<std::uint32_t>(nowMs - s[i].spare) >= afterMs) {
                s[i].id[kIdBytes - 1] = '\0';
                try {
                    due.emplace_back(s[i].id);
                } catch (...) {
                    return;
                }
            }
        }
    }
    for (const std::string& id : due) { set(id, Phase::Running); }
}

std::set<std::string> ranThisSession() {
    State& st = state();
    std::lock_guard<std::mutex> g(st.ranMutex);
    return st.ran;
}

std::size_t dropped() noexcept { return state().dropped.load(std::memory_order_relaxed); }

void resetForTest() {
    State& st = state();
    g_open.store(nullptr, std::memory_order_release);
    SpinLock lk(st.lock);
    unmap(st);
    std::memset(st.memory, 0, sizeof st.memory);
    st.dropped.store(0);
    std::lock_guard<std::mutex> g(st.ranMutex);
    st.ran.clear();
}

}  // namespace marker

// ---------------------------------------------------------------------------------------------------------------------------------
// The strikes
// ---------------------------------------------------------------------------------------------------------------------------------

const char* strikeStateWord(StrikeState s) {
    switch (s) {
        case StrikeState::Suspect: return "suspect";
        case StrikeState::Probation: return "probation";
        case StrikeState::Disabled: return "disabled";
    }
    return "suspect";
}

std::vector<marker::InPlay> suspectsOf(const std::vector<marker::InPlay>& previous) {
    using marker::Phase;
    std::vector<marker::InPlay> out;
    std::vector<marker::InPlay> firstPass;
    for (const marker::InPlay& e : previous) {
        if (e.blamed || e.phase == Phase::Load || e.phase == Phase::Create || e.phase == Phase::Unload) {
            out.push_back(e);
        } else if (e.phase == Phase::FirstPass) {
            firstPass.push_back(e);
        }
    }
    // Two plugins that were both created and not yet fed prove nothing about either; one alone, with nothing else suspect, is the evidence
    // there is.
    if (out.empty() && firstPass.size() == 1) { out.push_back(firstPass.front()); }
    return out;
}

StartPlan planStart(const std::vector<StrikeRecord>& stored, const std::vector<marker::InPlay>& previous, std::int64_t nowUnix) {
    StartPlan plan;
    plan.records = stored;
    const std::vector<marker::InPlay> suspects = suspectsOf(previous);
    for (const marker::InPlay& s : suspects) { plan.suspects.push_back(s.id); }

    auto isSuspect = [&](const std::string& id) {
        return std::any_of(suspects.begin(), suspects.end(), [&](const marker::InPlay& s) { return s.id == id; });
    };
    // 1. A plugin that was skipped for one start gets its second chance now (and one that was not suspected again stays as it is).
    for (StrikeRecord& r : plan.records) {
        if (r.state == StrikeState::Suspect && !isSuspect(r.id)) { r.state = StrikeState::Probation; }
    }
    // 2. The new suspicions.
    for (const marker::InPlay& s : suspects) {
        StrikeRecord* r = nullptr;
        for (StrikeRecord& x : plan.records) {
            if (x.id == s.id) { r = &x; }
        }
        if (r == nullptr) {
            plan.records.push_back(StrikeRecord{});
            r = &plan.records.back();
            r->id = s.id;
        }
        r->strikes += 1;
        r->version = s.version;
        r->phase = marker::phaseWord(s.phase);
        r->whenUnix = nowUnix;
        r->fromSite = s.origin == fault::Origin::Site;
        r->state = r->strikes >= 2 ? StrikeState::Disabled : StrikeState::Suspect;
        StartNotice n;
        n.id = r->id;
        n.version = r->version;
        n.strikes = r->strikes;
        n.permanent = r->state == StrikeState::Disabled;
        n.phase = r->phase;
        plan.notices.push_back(std::move(n));
    }
    for (const StrikeRecord& r : plan.records) {
        if (r.state == StrikeState::Suspect || r.state == StrikeState::Disabled) { plan.skip.push_back(r.id); }
    }
    return plan;
}

std::vector<StrikeRecord> graduate(const std::vector<StrikeRecord>& stored, const std::set<std::string>& ran) {
    std::vector<StrikeRecord> out;
    for (const StrikeRecord& r : stored) {
        if (r.state == StrikeState::Probation && ran.count(r.id) != 0) { continue; }
        out.push_back(r);
    }
    return out;
}

bool loadStore(const std::string& path, std::vector<StrikeRecord>& out, std::string& error) {
    out.clear();
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) { return true; }   // no file: an empty store, which is the normal case
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("plugins") || !j["plugins"].is_array()) {
        error = "the plugin fault record is not readable and was ignored";
        return false;
    }
    for (const nlohmann::json& e : j["plugins"]) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_string()) { continue; }
        StrikeRecord r;
        r.id = e["id"].get<std::string>();
        if (r.id.empty() || r.id.size() >= 48) { continue; }
        r.version = e.value("version", std::string());
        if (r.version.size() > 23) { r.version.resize(23); }
        r.strikes = e.value("strikes", 1u);
        const std::string st = e.value("state", std::string("suspect"));
        r.state = st == "disabled" ? StrikeState::Disabled : (st == "probation" ? StrikeState::Probation : StrikeState::Suspect);
        r.phase = e.value("phase", std::string());
        r.whenUnix = e.value("when", std::int64_t{0});
        r.fromSite = e.value("site", false);
        out.push_back(std::move(r));
    }
    return true;
}

bool saveStore(const std::string& path, const std::vector<StrikeRecord>& records, std::string& error) {
    error.clear();
    nlohmann::json j = nlohmann::json::object();
    j["schema"] = 1;
    nlohmann::json arr = nlohmann::json::array();
    for (const StrikeRecord& r : records) {
        nlohmann::json e = nlohmann::json::object();
        e["id"] = r.id;
        e["version"] = r.version;
        e["strikes"] = r.strikes;
        e["state"] = strikeStateWord(r.state);
        e["phase"] = r.phase;
        e["when"] = r.whenUnix;
        e["site"] = r.fromSite;
        arr.push_back(std::move(e));
    }
    j["plugins"] = std::move(arr);
    const std::string text = j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    namespace fs = std::filesystem;
    std::error_code ec;
    if (records.empty()) {
        // An empty store is no file at all: a clean install leaves nothing behind.
        fs::remove(path, ec);
        return true;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "the plugin fault record could not be written";
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = "the plugin fault record could not be written";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "the plugin fault record could not be replaced";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Containment
// ---------------------------------------------------------------------------------------------------------------------------------

void Containment::begin(const std::string& dir, bool safeMode, std::int64_t nowUnix) {
    dir_ = dir;
    safeMode_ = safeMode;
    started_ = true;
    plan_ = StartPlan{};
    records_.clear();
    namespace fs = std::filesystem;
    const std::string markerPath = dir.empty() ? std::string() : (fs::path(dir) / marker::kFileName).string();
    const std::string storePath = dir.empty() ? std::string() : (fs::path(dir) / kStoreFileName).string();

    std::vector<StrikeRecord> stored;
    if (!storePath.empty()) {
        std::string err;
        if (!loadStore(storePath, stored, err)) { diagWarnf("plugins: %s", err.c_str()); }
    }
    const std::vector<marker::InPlay> previous = marker::open(markerPath);
    if (safeMode) {
        // THE ESCAPE HATCH: nothing is loaded, and nothing is learned from what the last run left - a run that is being started to find out
        // whether the breaker is the problem must not move the breaker's state. (The marker is still opened, for this run's own use.)
        records_ = stored;
        diagLogf("plugins: safe mode - no plugin will be loaded this session");
        return;
    }
    plan_ = planStart(stored, previous, nowUnix);
    records_ = plan_.records;
    for (const std::string& id : plan_.suspects) { diagWarnf("plugins: the last run did not end in order with %s in play", id.c_str()); }
    for (const StartNotice& n : plan_.notices) {
        diagWarnf("plugins: %s %s (strike %u of 2) - %s", n.id.c_str(), n.permanent ? "is switched off until it is re-enabled" : "starts the next session switched off",
                  n.strikes, n.phase.c_str());
    }
    persist();
}

bool Containment::gate(const std::string& pluginKeyOrId, Gate& out) const {
    if (!started_) { return false; }
    if (safeMode_) {
        out.verdict = ModuleVerdict::SafeMode;
        out.sentence =
            "Not loaded: FoxSDR was started in safe mode (--safe-plugins, or the Shift key held at launch), so no plugin is loaded this session. Start it normally to load them.";
        return true;
    }
    const StrikeRecord* r = recordOf(pluginKeyOrId);
    if (r == nullptr) { return false; }
    if (r->state == StrikeState::Suspect) {
        out.verdict = ModuleVerdict::Faulted;
        out.sentence =
            "Not loaded: FoxSDR stopped unexpectedly with this plugin in use, so this session started without it. It will be given another chance "
            "next time, or RE-ENABLE it now.";
        return true;
    }
    if (r->state == StrikeState::Disabled) {
        out.verdict = ModuleVerdict::Faulted;
        out.sentence =
            "Not loaded: FoxSDR stopped unexpectedly twice with this plugin in use, so it stays switched off until you RE-ENABLE it.";
        return true;
    }
    return false;
}

const StrikeRecord* Containment::recordOf(const std::string& pluginKeyOrId) const {
    std::string id = pluginRunId(pluginKeyOrId);
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    for (const StrikeRecord& r : records_) {
        if (r.id == id) { return &r; }
    }
    return nullptr;
}

bool Containment::reenable(const std::string& pluginKeyOrId) {
    std::string id = pluginRunId(pluginKeyOrId);
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    for (StrikeRecord& r : records_) {
        if (r.id != id) { continue; }
        if (r.state == StrikeState::Probation) { return false; }   // already enabled
        // Back on probation, strikes kept: a plugin the person switched on by hand and that is suspected again is strike two.
        r.state = StrikeState::Probation;
        persist();
        return true;
    }
    return false;
}

void Containment::persist() {
    if (dir_.empty()) { return; }
    std::string err;
    const std::string path = (std::filesystem::path(dir_) / kStoreFileName).string();
    if (!saveStore(path, records_, err)) { diagWarnf("plugins: %s", err.c_str()); }
}

void Containment::cleanShutdown() {
    if (!started_) { return; }
    if (!safeMode_) {
        const std::vector<StrikeRecord> after = graduate(records_, marker::ranThisSession());
        if (after.size() != records_.size()) {
            records_ = after;
            persist();
        }
    }
    marker::cleanShutdown();
}

}  // namespace cascade::core
