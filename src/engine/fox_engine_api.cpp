// fox_engine_api.cpp - see fox_engine_api.hpp. The session machinery - sessions,
// grants, logins and tokens, tickets, BUSY, the merge of the safer commands,
// results in ticket order, events - follows the reference engine (third_party/
// foxsdr_api/reference/mock_engine/mock_engine.cpp) rule for rule, and the
// review round each rule answers is named where the reference names it. What
// differs is what is behind it: the real receiver, applied through the one
// control path every client shares (Engine::applyCommand).
//
// NO ALLOCATION PER PASS ON THE STEADY PATH: each session's results and
// events live in fixed rings sized at open (FOXAPI_MAX_PENDING +
// FOXAPI_SAFETY_RESERVE results, 256 events), the queue is two vectors that
// swap, and the pass's scratch (outbox, live sessions) is reserved once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/fox_engine_api.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/app_commands.hpp"
#include "core/tx_key_rules.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/seqlock_box.hpp"
#include "core/utf8_text.hpp"
#include "engine/engine.hpp"
#include "engine/receiver_tables.hpp"
#include "net/status_compose.hpp"
#include "source/tx_sink.hpp"
#include "engine/tune_control.hpp"

// The C handles are opaque to callers; inside the engine they are these.
struct FoxEngine {};
struct FoxSession {};

namespace cascade::engine {

namespace {

using SteadyClock = std::chrono::steady_clock;

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}

std::int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

template <std::size_t N>
void copyText(char (&dst)[N], const char* src) {
    cascade::core::formatUtf8(dst, N, "%s", src != nullptr ? src : "");
}

// Rule 4: the other side's structSize is read or written, never compared
// with our own sizeof; `minSize` has been checked by the caller.
template <class T>
int32_t copyOut(const T& src, T* dst, std::size_t minSize) {
    const std::size_t want = dst->structSize;
    const std::size_t n = std::min(want, sizeof(T));
    (void)minSize;
    std::memcpy(dst, &src, n);
    dst->structSize = static_cast<uint32_t>(n);
    return FOXAPI_OK;
}

template <class T>
T copyInAt(const T* base, std::size_t stride, std::size_t i) {
    T v{};
    const auto* p = reinterpret_cast<const unsigned char*>(base) + stride * i;
    std::memcpy(&v, p, std::min(stride, sizeof(T)));
    v.structSize = sizeof(T);
    return v;
}

template <class T>
void copyOutAt(const T& src, T* base, std::size_t stride, std::size_t i) {
    auto* p = reinterpret_cast<unsigned char*>(base) + stride * i;
    const std::size_t n = std::min(stride, sizeof(T));
    std::memcpy(p, &src, n);
    const uint32_t filled = static_cast<uint32_t>(n);
    std::memcpy(p, &filled, sizeof(filled));  // structSize: the bytes filled
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    int pttHoldMs = FOXAPI_PTT_HOLD_MS;
    int latchTimeoutMs = FOXAPI_LATCH_TIMEOUT_MS;
    int keepaliveMs = FOXAPI_KEEPALIVE_MS;
    int remoteKeepaliveMs = FOXAPI_REMOTE_KEEPALIVE_MS;
    std::string token;  // a configured token; empty = only login() tokens
    std::string user;   // login() credential; both empty = login refused
    std::string password;
    int maxSessions = 64;
    int maxRemoteSessions = 16;
    int loginLockoutMs = 60000;
    std::int64_t tokenTtlMs = std::int64_t{12} * 3600 * 1000;
    bool testHooks = false;
    bool running = true;  // create(): start the receiver (the reference engine's default)
};

Options parseOptions(const std::string& all) {
    Options o;
    std::size_t pos = 0;
    while (pos <= all.size()) {
        std::size_t end = all.find(';', pos);
        if (end == std::string::npos) { end = all.size(); }
        const std::string kv = all.substr(pos, end - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq);
            const std::string v = kv.substr(eq + 1);
            const double d = std::atof(v.c_str());
            // A test may SHORTEN a safety timer. Nothing may lengthen one.
            if (k == "pttHoldMs") {
                o.pttHoldMs = std::clamp(static_cast<int>(d), 10, FOXAPI_PTT_HOLD_MS);
            } else if (k == "latchTimeoutMs") {
                o.latchTimeoutMs = std::clamp(static_cast<int>(d), 10, FOXAPI_LATCH_TIMEOUT_MS);
            } else if (k == "keepaliveMs") {
                o.keepaliveMs = std::clamp(static_cast<int>(d), 10, FOXAPI_KEEPALIVE_MS);
            } else if (k == "remoteKeepaliveMs") {
                o.remoteKeepaliveMs = std::clamp(static_cast<int>(d), 10, FOXAPI_REMOTE_KEEPALIVE_MS);
            } else if (k == "token") {
                o.token = v;
            } else if (k == "user") {
                o.user = v;
            } else if (k == "password") {
                o.password = v;
            } else if (k == "maxSessions") {
                o.maxSessions = std::clamp(static_cast<int>(d), 1, 1024);
            } else if (k == "maxRemoteSessions") {
                o.maxRemoteSessions = std::clamp(static_cast<int>(d), 1, 1024);
            } else if (k == "loginLockoutMs") {
                o.loginLockoutMs = std::clamp(static_cast<int>(d), 10, 60000);
            } else if (k == "tokenTtlMs") {
                o.tokenTtlMs = std::clamp(static_cast<std::int64_t>(d), std::int64_t{10},
                                          std::int64_t{12} * 3600 * 1000);
            } else if (k == "testHooks") {
                o.testHooks = d != 0.0;
            } else if (k == "running") {
                o.running = d != 0.0;
            }
        }
        pos = end + 1;
    }
    return o;
}

constexpr std::size_t kMaxPendingPerSession = FOXAPI_MAX_PENDING;
constexpr int kSaferKinds = 5;
static_assert(kSaferKinds == static_cast<int>(FOXAPI_SAFETY_RESERVE),
              "one outstanding safer command per kind is the whole reserve");
constexpr int kLocalReservedPlaces = 4;
constexpr std::size_t kMaxEventsPerSession = 256;
constexpr int kLoginFailuresBeforeLockout = 5;
constexpr std::uint32_t kMaxSpectrumBins = 16384;

// A FIXED RING: a bounded FIFO allocated once, so a pass never allocates for
// a result or an event (the reference engine's std::deque did, per push).
template <class T>
class FixedRing {
public:
    explicit FixedRing(std::size_t cap) : buf_(cap) {}
    bool full() const { return count_ == buf_.size(); }
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    void push_back(const T& v) {
        buf_[(head_ + count_) % buf_.size()] = v;
        ++count_;
    }
    T& front() { return buf_[head_]; }
    void pop_front() {
        head_ = (head_ + 1) % buf_.size();
        --count_;
    }
    T& at(std::size_t i) { return buf_[(head_ + i) % buf_.size()]; }
    void release() {
        std::vector<T>().swap(buf_);
        head_ = count_ = 0;
    }

private:
    std::vector<T> buf_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------
// The spectrum stream: a ring of slots written by the pumping thread (never
// the DSP thread: it copies the pipeline's newest frame once a pass), read
// lock-free by any interface - a reader overtaken mid-copy notices and
// retries, and the writer never waits for anyone. (The reference engine's
// SpectrumRing.)
// ---------------------------------------------------------------------------

class SpectrumRing {
public:
    static constexpr int kSlots = 4;

    SpectrumRing() {
        for (auto& s : slots_) {
            s.data = std::make_unique<std::atomic<float>[]>(kMaxSpectrumBins);
        }
    }

    void publish(const float* bins, std::size_t count, double centreHz, double spanHz, std::int64_t ums) {
        const uint64_t seq = ++lastSeq_;
        Slot& s = slots_[seq % kSlots];
        s.seq.store(0, std::memory_order_relaxed);  // "being written"
        std::atomic_thread_fence(std::memory_order_release);
        // More bins than a slot holds (never, at the engine's FFT size): the
        // MAXIMUM of each group, as a reduced read gives.
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(count, kMaxSpectrumBins));
        if (count <= kMaxSpectrumBins) {
            for (std::uint32_t i = 0; i < n; ++i) { s.data[i].store(bins[i], std::memory_order_relaxed); }
        } else {
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::size_t a = static_cast<std::size_t>(std::uint64_t(i) * count / n);
                std::size_t b = static_cast<std::size_t>(std::uint64_t(i + 1) * count / n);
                b = std::max(b, a + 1);
                float m = -1e30f;
                for (std::size_t k = a; k < b; ++k) { m = std::max(m, bins[k]); }
                s.data[i].store(m, std::memory_order_relaxed);
            }
        }
        s.bins.store(n, std::memory_order_relaxed);
        s.centreHz.store(centreHz, std::memory_order_relaxed);
        s.spanHz.store(spanHz, std::memory_order_relaxed);
        s.unixMs.store(ums, std::memory_order_relaxed);
        s.seq.store(seq, std::memory_order_release);
        latest_.store(seq, std::memory_order_release);
    }

    int32_t read(uint64_t since, FoxSpectrumInfo& info, float* out, uint32_t cap) const {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint64_t seq = latest_.load(std::memory_order_acquire);
            if (seq == 0 || seq <= since) { return FOXAPI_NO_CHANGE; }
            const Slot& s = slots_[seq % kSlots];
            if (s.seq.load(std::memory_order_acquire) != seq) { continue; }
            const uint32_t n = s.bins.load(std::memory_order_relaxed);
            info.binCount = n;
            info.seq = seq;
            info.centreHz = s.centreHz.load(std::memory_order_relaxed);
            info.spanHz = s.spanHz.load(std::memory_order_relaxed);
            info.unixMs = s.unixMs.load(std::memory_order_relaxed);
            info.flags = 0;
            const uint32_t outN = (out == nullptr) ? 0u : std::min(cap, n);
            if (outN == n) {
                for (uint32_t i = 0; i < n; ++i) { out[i] = s.data[i].load(std::memory_order_relaxed); }
            } else if (outN > 0) {
                // Fewer bins than the engine has: the MAXIMUM of each group,
                // so a narrow carrier survives the reduction.
                for (uint32_t i = 0; i < outN; ++i) {
                    const uint32_t a = static_cast<uint32_t>(uint64_t(i) * n / outN);
                    uint32_t b = static_cast<uint32_t>(uint64_t(i + 1) * n / outN);
                    b = std::max(b, a + 1);
                    float m = -1e30f;
                    for (uint32_t k = a; k < b; ++k) { m = std::max(m, s.data[k].load(std::memory_order_relaxed)); }
                    out[i] = m;
                }
            }
            info.copied = outN;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) == seq) { return FOXAPI_OK; }
        }
        return FOXAPI_BUSY;
    }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint32_t> bins{0};
        std::atomic<double> centreHz{0.0};
        std::atomic<double> spanHz{0.0};
        std::atomic<std::int64_t> unixMs{0};
        std::unique_ptr<std::atomic<float>[]> data;
    };
    std::array<Slot, kSlots> slots_;
    std::atomic<uint64_t> latest_{0};
    uint64_t lastSeq_ = 0;  // writer only
};

// THE TEST TRANSMITTER ("test:tx:dummy", an engine created with testHooks=1
// only): a dummy load that takes every block and radiates nothing - the
// reference engine's "mock:tx:dummy", so the key rules can be held to their
// tests with the real core::Transmitter and its TX thread in the loop.
class DummyLoad final : public cascade::source::IqSink {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_.load(); }
    double sampleRateHz() const override { return 2.5e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return hz_.load(); }
    bool setCenterFrequencyHz(double hz) override {
        hz_.store(hz);
        return true;
    }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return gain_.load(); }
    bool setGainDb(double db) override {
        gain_.store(std::clamp(db, -89.75, 0.0));
        return true;
    }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_.load() ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "Test transmitter (dummy load, no RF)"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<bool> running_{false};
    std::atomic<double> hz_{145.5e6};
    std::atomic<double> gain_{-89.75};
};

std::unique_ptr<cascade::source::IqSink> makeDummyLoad() { return std::make_unique<DummyLoad>(); }

// The group bits of FOXAPI_EVENT_STATE's value[0] (the reference engine's).
enum Group : uint32_t {
    kTune = 1u << 0,
    kMode = 1u << 1,
    kDevice = 1u << 2,
    kAudio = 1u << 3,
    kDisplay = 1u << 4,
    kTx = 1u << 5,
    kList = 1u << 6,
};

}  // namespace

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

struct ApiSession : FoxSession {
    ApiSession() : results(kMaxPendingPerSession + FOXAPI_SAFETY_RESERVE), events(kMaxEventsPerSession) {}

    uint64_t id = 0;
    uint32_t flags = 0;
    uint64_t grants = 0;
    std::string clientName;
    ApiHostImpl* host = nullptr;
    std::atomic<std::int64_t> lastBeatMs{0};
    std::string token;
    // WHO this session acts for: "local" for every LOCAL session, and
    // "token:<token>" for a remote one. Latch marks belong to the principal.
    std::string principal;
    std::atomic<bool> detached{false};

    std::mutex m;  // guards everything below, down to `safer`
    FixedRing<FoxCommandResult> results;
    FixedRing<FoxEvent> events;
    uint64_t eventMask = 0;
    uint64_t eventSeq = 0;
    uint64_t eventsDropped = 0;
    std::size_t pending = 0;
    uint64_t lastTicket = 0;
    struct SaferSlot {
        uint64_t ticket = 0;
        int copies = 0;
        bool inResults = false;
        bool read = false;
    };
    std::array<SaferSlot, kSaferKinds> safer{};

    std::atomic<bool> releaseFirst{false};
};

struct Pending {
    FoxCommand cmd;
    uint64_t ticket;
    uint64_t sessionId;
    uint32_t sessionFlags;
    int32_t refused;
    int saferKind = -1;
    bool merged = false;
    std::uint64_t sourceGen = 0;  // Engine::sourceGen_ when submitted (device-scoped ops)
    std::string principal;        // ApiSession::principal of the sender ("local" needs no allocation)
    std::string longText;         // submit_text's text (API 0.3); empty for every other command
};

// ---------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------

class ApiHostImpl : public FoxEngine {
public:
    ApiHostImpl(Engine& engine, const std::string& options)
        : engine_(engine), opt_(parseOptions(options)), rules_(timersFrom(opt_)), sessionsView_(*this) {
        outbox_.reserve(1024);
        live_.reserve(64);
        queue_.reserve(256);
        work_.reserve(256);
        // The web remote's key (engine-internal, REMOTE, never listed): the
        // one session the app's browser page holds. Its heartbeat is the
        // page's status poll (webRemoteBeat).
        webSession_.id = kWebSessionId;
        webSession_.principal = "web";
        if (opt_.testHooks) { engine_.txSinkForTest_ = &makeDummyLoad; }
    }

    ~ApiHostImpl() = default;

    Engine& engine() { return engine_; }
    const Options& options() const { return opt_; }
    Engine* owned = nullptr;  // create(): the Engine this handle owns (destroy deletes it)

    // ---- sessions ----------------------------------------------------------

    int32_t openSession(const FoxSessionParams* p, FoxSession** out) {
        if (p == nullptr || out == nullptr || p->structSize < FOXAPI_MIN_SESSION_PARAMS) {
            return FOXAPI_BAD_ARGUMENT;
        }
        const bool remote = (p->flags & FOXAPI_SESSION_REMOTE) != 0u;
        const bool local = (p->flags & FOXAPI_SESSION_LOCAL) != 0u;
        if (remote == local) { return FOXAPI_BAD_ARGUMENT; }
        uint64_t grants = p->grants | FOXAPI_GRANT_VIEW;
        std::string token;
        if (remote) {
            if (p->token == nullptr || !tokenValid(p->token)) { return FOXAPI_UNAUTHENTICATED; }
            token = p->token;
            grants &= ~FOXAPI_GRANT_ADMIN;  // administration is a local act
        }
        if (const int delay = openDelayMs_.load(std::memory_order_relaxed); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        auto s = std::make_shared<ApiSession>();
        s->flags = p->flags;
        s->grants = grants;
        s->clientName = p->clientName ? p->clientName : "";
        s->host = this;
        s->token = token;
        s->principal = remote ? "token:" + token : std::string("local");
        if (!remote) { s->releaseFirst.store(localMarked_.load(std::memory_order_relaxed)); }
        s->lastBeatMs.store(steadyMs());
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            const int open = static_cast<int>(sessions_.size());
            if (open >= opt_.maxSessions) { return FOXAPI_LIMIT; }
            if (remote) {
                if (open >= opt_.maxSessions - kLocalReservedPlaces) { return FOXAPI_LIMIT; }
                int remotes = 0;
                for (const auto& kv : sessions_) {
                    remotes += ((kv.second->flags & FOXAPI_SESSION_REMOTE) != 0u && !kv.second->detached.load()) ? 1
                                                                                                                 : 0;
                }
                if (remotes >= opt_.maxRemoteSessions) { return FOXAPI_LIMIT; }
            }
            s->id = ++nextSessionId_;
            sessions_[s->id] = s;
        }
        if (remote && !tokenValid(token.c_str())) {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            sessions_.erase(s->id);
            return FOXAPI_UNAUTHENTICATED;
        }
        *out = s.get();
        return FOXAPI_OK;
    }

    void closeSession(ApiSession* s) {
        std::shared_ptr<ApiSession> keep;
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            auto it = sessions_.find(s->id);
            if (it == sessions_.end()) {
                reaped_.erase(s->id);
                return;
            }
            keep = it->second;
            sessions_.erase(it);
        }
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            closedSessions_.push_back(keep->id);
        }
        queueCv_.notify_all();
        engine_.wakeControl();
    }

    // ---- login ---------------------------------------------------------------

    int32_t login(const char* user, const char* password, char* out, std::size_t cap) {
        if (user == nullptr || password == nullptr || out == nullptr || cap < 65) { return FOXAPI_BAD_ARGUMENT; }
        const std::int64_t now = steadyMs();
        std::lock_guard<std::mutex> lk(tokensMutex_);
        if (lockedUntilMs_ > now) { return FOXAPI_LIMIT; }
        if (opt_.user.empty() || opt_.password.empty() || opt_.user != user || opt_.password != password) {
            if (++loginFailures_ >= kLoginFailuresBeforeLockout) {
                loginFailures_ = 0;
                lockedUntilMs_ = now + opt_.loginLockoutMs;
            }
            return FOXAPI_UNAUTHENTICATED;
        }
        loginFailures_ = 0;
        std::random_device rd;
        static const char* hex = "0123456789abcdef";
        std::string t;
        for (int i = 0; i < 8; ++i) {
            uint32_t w = rd();
            for (int k = 0; k < 8; ++k) {
                t += hex[w & 0xF];
                w >>= 4;
            }
        }
        tokens_[t] = now + opt_.tokenTtlMs;
        std::memcpy(out, t.c_str(), t.size() + 1);
        return static_cast<int32_t>(t.size());
    }

    int32_t logout(const char* token) {
        if (token == nullptr) { return FOXAPI_BAD_ARGUMENT; }
        if (!opt_.token.empty() && opt_.token == token) { return FOXAPI_DENIED; }
        bool live = false;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            auto it = tokens_.find(token);
            live = it != tokens_.end() && steadyMs() < it->second;
        }
        const bool erased = dropToken(token);
        return (live && erased) ? FOXAPI_OK : FOXAPI_NOT_FOUND;
    }

    void detachToken(const std::string& token) {
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            for (auto& kv : sessions_) {
                if (kv.second->token == token) {
                    kv.second->detached.store(true);
                    ids.push_back(kv.first);
                }
            }
        }
        if (ids.empty()) { return; }
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            closedSessions_.insert(closedSessions_.end(), ids.begin(), ids.end());
        }
        queueCv_.notify_all();
        engine_.wakeControl();
    }

    void expireTokens() {
        std::vector<std::string> gone;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            if (tokens_.empty()) { return; }
            const std::int64_t now = steadyMs();
            for (const auto& kv : tokens_) {
                if (now >= kv.second) { gone.push_back(kv.first); }
            }
        }
        for (const std::string& t : gone) { dropToken(t); }
    }

    bool tokenValid(const char* t) {
        if (!opt_.token.empty() && opt_.token == t) { return true; }
        bool expired = false;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            auto it = tokens_.find(t);
            if (it == tokens_.end()) { return false; }
            if (steadyMs() < it->second) { return true; }
            expired = true;
        }
        if (expired) { dropToken(t); }
        return false;
    }

    bool dropToken(const std::string& token) {
        bool erased = false;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            erased = tokens_.erase(token) != 0;
        }
        detachToken(token);
        return erased;
    }

    int32_t heartbeat(ApiSession* s) {
        s->lastBeatMs.store(steadyMs(), std::memory_order_relaxed);
        // THE TX THREAD'S OWN DEAD-MAN, pointed at the key holder: its
        // heartbeat, and nobody else's, is the front-end stamp the thread
        // watches (core/transmitter.hpp).
        if (s->id == keyOwner_.load(std::memory_order_relaxed)) { engine_.transmitter_.frontEndAlive(); }
        return FOXAPI_OK;
    }

    // ---- THE KEY (core/tx_key_rules.hpp) ---------------------------------------
    // Everything below runs on the pumping thread, except webRemoteBeat.

    // An end the ENGINE makes (Engine::applyCommand's RUN 0, TX_OPEN,
    // TX_CLOSE - whichever client sent them - and a fault).
    void endKeyFromEngine(const char* why) { rules_.endKey(why, steadyMs()); }

    // The web remote's PTT (Engine::applyCommand's TX_PTT: the browser's
    // transmitPtt arrives there through applyControlRequest). A REMOTE key:
    // a 2 s hold the page re-asserts, only with the local operator's consent.
    cascade::core::TxKeyAnswer webRemotePtt(bool on) {
        cascade::core::TxKeySender snd{kWebSessionId, true, webSession_.principal, true};
        const cascade::core::TxKeyAnswer a = rules_.key(snd, false, on, engine_.transmitter_.haveSink(), steadyMs());
        if (on && a.status == FOXAPI_OK) { engine_.transmitter_.keyRemote(); }
        // PTT 0 RELEASES NOW (API 0.2): in this step, not at the next pass.
        if (!on && rules_.keyOwner() != kWebSessionId) { engine_.transmitter_.releaseRemote("the remote let go"); }
        return a;
    }

    // The web server is not running: no browser can ask again, so a key the
    // web remote holds ends now rather than at its hold.
    void webRemoteStopped(const char* why) {
        if (rules_.keyOwner() == kWebSessionId) { rules_.endKey(why, steadyMs()); }
        engine_.transmitter_.releaseRemote(why);
    }

    // Any thread: the browser page polled its status - the web remote's
    // heartbeat (its PTT re-assertions are not one).
    void webRemoteBeat() { webBeatMs_.store(steadyMs(), std::memory_order_relaxed); }

    // The control loop's last act, before it stops the Transmitter.
    void engineStopping() { rules_.endKey("the engine is shutting down", steadyMs()); }

    ApiHost::TxFacts txFacts() const {
        ApiHost::TxFacts f;
        const std::int64_t now = steadyMs();
        f.owner = rules_.keyOwner();
        f.latched = rules_.latched();
        f.remoteArmed = rules_.remoteArmedBy() != 0;
        f.holdRemainingMs = rules_.holdRemainingMs(now);
        f.latchRemainingMs = rules_.latchRemainingMs(now);
        f.unkeyReason = rules_.unkeyReason().c_str();
        f.changes = rules_.changes();
        return f;
    }

    // ---- reads -------------------------------------------------------------

    int32_t readState(ApiSession* s, FoxReceiverState* out) {
        if (out == nullptr || out->structSize < FOXAPI_MIN_RECEIVER_STATE) { return FOXAPI_BAD_ARGUMENT; }
        cascade::core::PublishedState p;
        if (!engine_.receiverSnapshot_->read(p)) { return FOXAPI_BUSY; }
        FoxReceiverState st = p.rx;
        st.structSize = sizeof(st);
        st.grants = s->grants;
        if (p.app.txKeyOwner != 0 && p.app.txKeyOwner == s->id) { st.flags |= FOXAPI_RX_TX_KEY_MINE; }
        if (s->releaseFirst.load(std::memory_order_relaxed)) { st.flags |= FOXAPI_RX_TX_LATCH_RELEASE_FIRST; }
        return copyOut(st, out, FOXAPI_MIN_RECEIVER_STATE);
    }

    int32_t readSpectrum(uint64_t since, FoxSpectrumInfo* info, float* bins, uint32_t cap) {
        if (info == nullptr || info->structSize < FOXAPI_MIN_SPECTRUM_INFO || (bins == nullptr && cap != 0)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        FoxSpectrumInfo local{};
        local.structSize = sizeof(local);
        const int32_t r = spectrum_.read(since, local, bins, cap);
        if (r != FOXAPI_OK) { return r; }
        copyOut(local, info, FOXAPI_MIN_SPECTRUM_INFO);
        return FOXAPI_OK;
    }

    int32_t readList(const ApiSession* s, uint32_t list, uint32_t index, FoxListItem* out);

    int32_t getSetting(ApiSession* s, const char* key, char* buf, std::size_t cap) {
        if (key == nullptr || (buf == nullptr && cap != 0)) { return FOXAPI_BAD_ARGUMENT; }
        std::string v;
        if (opt_.testHooks && std::strcmp(key, "test.sessionId") == 0) {
            v = std::to_string(s->id);  // test seam: which id the key state names this session by
        } else if (opt_.testHooks && std::strcmp(key, "test.queueDepth") == 0) {
            std::lock_guard<std::mutex> lk(queueMutex_);
            v = std::to_string(queue_.size());
        } else if (opt_.testHooks && std::strcmp(key, "test.keyState") == 0) {
            std::lock_guard<std::mutex> lk(keyStateMutex_);
            v = keyState_;
        } else if (std::strncmp(key, "fact.", 5) == 0) {
            // API 0.3's read-only facts, from the list block the pumping
            // thread built (docs/api-0.3-additions.md).
            std::shared_ptr<const ApiListBlock> b;
            {
                std::lock_guard<std::mutex> lk(listsMutex_);
                b = lists_;
            }
            if (b == nullptr) { return FOXAPI_NOT_FOUND; }
            const auto it = b->facts.find(key + 5);
            if (it == b->facts.end()) { return FOXAPI_NOT_FOUND; }
            if (it->second.localOnly && (s->flags & FOXAPI_SESSION_LOCAL) == 0u) { return FOXAPI_DENIED; }
            v = it->second.value;
        } else {
            return FOXAPI_NOT_FOUND;
        }
        if (cap > 0) {
            const std::size_t n = std::min(v.size(), cap - 1);
            std::memcpy(buf, v.data(), n);
            buf[n] = '\0';
        }
        return static_cast<int32_t>(v.size());
    }

    // ---- commands ----------------------------------------------------------

    int32_t submit(ApiSession* s, const FoxCommand* cmds, uint32_t count, FoxSubmitResult* results,
                   const std::string* longText = nullptr) {
        if (count == 0) { return 0; }
        if (count > FOXAPI_MAX_BATCH) { return FOXAPI_LIMIT; }
        if (cmds == nullptr || cmds[0].structSize < FOXAPI_MIN_COMMAND) { return FOXAPI_BAD_ARGUMENT; }
        const std::size_t stride = cmds[0].structSize;
        std::size_t rstride = 0;
        if (results != nullptr) {
            rstride = results[0].structSize;
            if (rstride < FOXAPI_MIN_SUBMIT_RESULT) { return FOXAPI_BAD_ARGUMENT; }
        }
        if (!s->token.empty() && !tokenValid(s->token.c_str())) {
            detachToken(s->token);
            return FOXAPI_DETACHED;
        }
        cascade::core::PublishedState snap;
        const bool haveSnap = engine_.receiverSnapshot_->read(snap);
        const std::uint64_t gen = engine_.sourceGen_.load();
        int32_t taken = 0;
        std::lock_guard<std::mutex> lk(s->m);
        std::lock_guard<std::mutex> lq(queueMutex_);
        for (uint32_t i = 0; i < count; ++i) {
            const FoxCommand c = copyInAt<FoxCommand>(cmds, stride, i);
            FoxSubmitResult r{};
            r.structSize = sizeof(r);
            const int32_t refused = validate(*s, c, haveSnap ? &snap.rx : nullptr);
            const int kind = refused == FOXAPI_OK ? saferKind(c) : -1;
            if (kind < 0 && s->pending >= kMaxPendingPerSession) {
                r.status = FOXAPI_BUSY;
            } else {
                Pending p{c, 0, s->id, s->flags, refused, kind, false, gen, s->principal, {}};
                if (longText != nullptr && refused == FOXAPI_OK) { p.longText = *longText; }
                if (kind < 0) {
                    r.status = FOXAPI_OK;
                    r.ticket = ++s->lastTicket;
                    ++s->pending;
                    ++taken;
                } else {
                    ApiSession::SaferSlot& slot = s->safer[static_cast<std::size_t>(kind)];
                    if (slot.ticket != 0 && !slot.read) {
                        r.status = FOXAPI_NO_CHANGE;
                        r.ticket = slot.ticket;
                        p.merged = true;
                    } else {
                        slot = ApiSession::SaferSlot{};
                        slot.ticket = ++s->lastTicket;
                        r.status = FOXAPI_OK;
                        r.ticket = slot.ticket;
                        ++taken;
                    }
                }
                p.ticket = r.ticket;
                ++submitted_;
                enqueueLocked(*s, p);
            }
            if (results != nullptr) { copyOutAt(r, results, rstride, i); }
        }
        queueCv_.notify_all();
        engine_.wakeControl();
        return taken;
    }

    // API 0.3: ONE command whose text is `text` rather than its text[]
    // slot. Validated on the slot's first 255 bytes, as the long-text
    // channel inside the app always has been (core/app_commands.hpp); applied
    // with the whole text.
    int32_t submitText(ApiSession* s, const FoxCommand* cmd, const char* text, uint32_t len, FoxSubmitResult* result) {
        if (cmd == nullptr || cmd->structSize < FOXAPI_MIN_COMMAND || (text == nullptr && len != 0)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        if (len > FOXAPI_MAX_LONG_TEXT) { return FOXAPI_LIMIT; }
        FoxCommand c = copyInAt<FoxCommand>(cmd, cmd->structSize, 0);
        c.structSize = sizeof(c);
        const std::string whole(text == nullptr ? "" : text, len);
        const std::size_t n = std::min<std::size_t>(len, FOXAPI_TEXT_CHARS - 1);
        std::memset(c.text, 0, sizeof(c.text));
        std::memcpy(c.text, whole.data(), n);
        const std::string empty;
        return submit(s, &c, 1, result, len > n ? &whole : &empty);
    }

    // API 0.3: the newest frames of a scope tap (core/scope_tap.hpp - its
    // snapshot is lock-free and any thread may take one).
    int32_t readScope(ApiSession* s, uint32_t tap, FoxScopeInfo* info, float* out, uint32_t cap) {
        if (info != nullptr && info->structSize < FOXAPI_MIN_SCOPE_INFO) { return FOXAPI_BAD_ARGUMENT; }
        if (out == nullptr && cap != 0) { return FOXAPI_BAD_ARGUMENT; }
        if ((s->flags & FOXAPI_SESSION_REMOTE) != 0u) { cap = std::min(cap, FOXAPI_SCOPE_REMOTE_FRAMES); }
        cascade::core::PublishedState ps;
        const bool have = engine_.receiverSnapshot_->read(ps);
        FoxScopeInfo local{};
        local.structSize = sizeof(local);
        local.tap = tap;
        std::size_t got = 0;
        const cascade::core::Pipeline& pl = engine_.pipeline_;
        switch (tap) {
            case FOXAPI_SCOPE_AUDIO:
                local.channels = 1;
                local.rateHz = cascade::core::Pipeline::kAudioRateHz;
                local.written = pl.scopeAudio().written();
                got = cap == 0 ? 0 : pl.scopeAudio().snapshot(out, cap);
                break;
            case FOXAPI_SCOPE_MPX:
                local.channels = 1;
                local.rateHz = have ? ps.rx.channelRateHz : 0.0;
                local.written = pl.scopeMpx().written();
                got = cap == 0 ? 0 : pl.scopeMpx().snapshot(out, cap);
                break;
            case FOXAPI_SCOPE_IQ:
                // std::complex<float> is two floats, laid out as an array of
                // them (the standard's own guarantee): copied straight in.
                local.channels = 2;
                local.rateHz = have ? ps.rx.channelRateHz : 0.0;
                local.written = pl.scopeIq().written();
                got = cap == 0 ? 0 : pl.scopeIq().snapshot(reinterpret_cast<std::complex<float>*>(out), cap);
                break;
            default:
                return FOXAPI_UNSUPPORTED;
        }
        if (info != nullptr) { copyOut(local, info, FOXAPI_MIN_SCOPE_INFO); }
        return static_cast<int32_t>(got);
    }

    // API 0.3: a patch radio's own spectrum, published once a pass beside the
    // main one (endPass), read exactly as read_spectrum reads that.
    int32_t readNodeSpectrum(uint64_t node, uint64_t since, FoxSpectrumInfo* info, float* bins, uint32_t cap) {
        if (info == nullptr || info->structSize < FOXAPI_MIN_SPECTRUM_INFO || (bins == nullptr && cap != 0)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        std::shared_ptr<SpectrumRing> ring;
        {
            std::lock_guard<std::mutex> lk(nodeSpectraMutex_);
            const auto it = nodeSpectra_.find(node);
            if (it == nodeSpectra_.end()) { return FOXAPI_NOT_FOUND; }
            ring = it->second;
        }
        FoxSpectrumInfo local{};
        local.structSize = sizeof(local);
        const int32_t r = ring->read(since, local, bins, cap);
        if (r != FOXAPI_OK) { return r; }
        copyOut(local, info, FOXAPI_MIN_SPECTRUM_INFO);
        return FOXAPI_OK;
    }

    // The pumping thread hands over the decoder-output backlog: the lines
    // since the last pass go into a FIXED ring (no allocation, whatever the
    // decoders say), cut to what a list item holds.
    void takeDecoderLines(const std::deque<cascade::core::DecodedLine>& log, std::uint64_t total) {
        if (total == decoderLinesSeen_) { return; }
        const std::uint64_t fresh = std::min<std::uint64_t>(total - decoderLinesSeen_, log.size());
        std::lock_guard<std::mutex> lk(decoderLinesMutex_);
        for (std::size_t i = log.size() - static_cast<std::size_t>(fresh); i < log.size(); ++i) {
            DecoderLine& d = decoderLines_[decoderLinesNext_ % kDecoderLines];
            d.number = total - (log.size() - i) + 1;
            copyText(d.plugin, log[i].plugin.c_str());
            copyText(d.text, log[i].text.c_str());
            ++decoderLinesNext_;
        }
        decoderLinesSeen_ = total;
    }

    int32_t pollResults(ApiSession* s, FoxCommandResult* out, uint32_t cap) {
        if (cap == 0) { return 0; }
        if (out == nullptr || out[0].structSize < FOXAPI_MIN_COMMAND_RESULT) { return FOXAPI_BAD_ARGUMENT; }
        const std::size_t stride = out[0].structSize;
        std::lock_guard<std::mutex> lk(s->m);
        uint32_t n = 0;
        while (n < cap && !s->results.empty()) {
            const FoxCommandResult& r = s->results.front();
            copyOutAt(r, out, stride, n);
            bool safer = false;
            for (ApiSession::SaferSlot& slot : s->safer) {
                if (slot.ticket != 0 && slot.ticket == r.ticket && slot.inResults) {
                    slot.inResults = false;
                    if (slot.copies == 0) {
                        slot = ApiSession::SaferSlot{};
                    } else {
                        slot.read = true;
                    }
                    safer = true;
                    break;
                }
            }
            if (!safer && s->pending > 0) { --s->pending; }
            s->results.pop_front();
            ++n;
        }
        return static_cast<int32_t>(n);
    }

    int32_t subscribe(ApiSession* s, uint64_t mask) {
        std::lock_guard<std::mutex> lk(s->m);
        s->eventMask = mask;
        return FOXAPI_OK;
    }

    int32_t pollEvents(ApiSession* s, FoxEvent* out, uint32_t cap) {
        if (cap == 0) { return 0; }
        if (out == nullptr || out[0].structSize < FOXAPI_MIN_EVENT) { return FOXAPI_BAD_ARGUMENT; }
        const std::size_t stride = out[0].structSize;
        std::lock_guard<std::mutex> lk(s->m);
        uint32_t n = 0;
        if (s->eventsDropped > 0) {
            FoxEvent e{};
            e.structSize = sizeof(e);
            e.kind = FOXAPI_EVENT_OVERFLOW;
            e.seq = ++s->eventSeq;
            e.unixMs = unixMs();
            e.value[0] = static_cast<double>(s->eventsDropped);
            copyOutAt(e, out, stride, n++);
            s->eventsDropped = 0;
        }
        while (n < cap && !s->events.empty()) {
            copyOutAt(s->events.front(), out, stride, n);
            s->events.pop_front();
            ++n;
        }
        return static_cast<int32_t>(n);
    }

    // ---- the pass (the pumping thread) -----------------------------------------

    void beginPass();
    void endPass();

private:
    friend class ApiHost;

    static bool finite(const FoxCommand& c) {
        for (double d : c.num) {
            if (!std::isfinite(d)) { return false; }
        }
        return true;
    }

    // TX_PTT 0, TX_LATCH 0, RUN 0, TX_CLOSE, TX_REMOTE_ARM 0.
    static int saferKind(const FoxCommand& c) {
        switch (c.op) {
            case FOXAPI_OP_TX_PTT: return c.ival[0] == 0 ? 0 : -1;
            case FOXAPI_OP_TX_LATCH: return c.ival[0] == 0 ? 1 : -1;
            case FOXAPI_OP_RUN: return c.ival[0] == 0 ? 2 : -1;
            case FOXAPI_OP_TX_CLOSE: return 3;
            case FOXAPI_OP_TX_REMOTE_ARM: return c.ival[0] == 0 ? 4 : -1;
            default: return -1;
        }
    }

    static bool onlyMakesSafer(const FoxCommand& c) { return saferKind(c) >= 0; }

    // Every send keeps its own place; a merged send that cannot change
    // anything is dropped (see the reference engine's enqueueLocked, and
    // test_engine_api_merge for the proof).
    void enqueueLocked(ApiSession& s, const Pending& p) {
        if (p.saferKind < 0) {
            queue_.push_back(p);
            return;
        }
        if (p.merged) {
            for (auto it = queue_.rbegin(); it != queue_.rend() && it->saferKind >= 0; ++it) {
                if (it->sessionId == p.sessionId && it->saferKind == p.saferKind) { return; }
            }
        }
        ++s.safer[static_cast<std::size_t>(p.saferKind)].copies;
        queue_.push_back(p);
    }

    static bool textTerminated(const FoxCommand& c) { return std::memchr(c.text, '\0', sizeof(c.text)) != nullptr; }

    // The grant each op needs (API.md section 5): TUNE for tuning, TRANSMIT
    // for the transmitter, ADMIN for administration, VIEW for the reports,
    // SETTINGS for the rest. The app's extension ops follow the same split.
    static uint64_t grantFor(uint32_t op) {
        switch (op) {
            case FOXAPI_OP_SET_CENTRE: case FOXAPI_OP_SET_FREQUENCY: case FOXAPI_OP_SET_VFO_OFFSET:
            case FOXAPI_OP_STEP_TUNE: case FOXAPI_OP_BOOKMARK_TUNE: case FOXAPI_OP_SCANNER_RUN:
            case FOXAPI_OP_SCANNER_SKIP: case FOXAPI_OP_SCANNER_CONFIG:
            case FOXAPP_OP_VFO_TO_ABSOLUTE: case FOXAPP_OP_SET_VFO_OFFSET_FREE: case FOXAPP_OP_SCANNER_RANGE:
                return FOXAPI_GRANT_TUNE;
            case FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN: case FOXAPP_OP_SET_TRANSMIT_ARGS:
                return FOXAPI_GRANT_TRANSMIT;
            case FOXAPI_OP_SETTING_SET: case FOXAPI_OP_TELEMETRY_ENABLE: case FOXAPI_OP_SERVER_CONFIG:
            case FOXAPI_OP_UPDATE_CHECK: case FOXAPI_OP_STORE_FETCH: case FOXAPI_OP_STORE_INSTALL:
            case FOXAPI_OP_STORE_REMOVE: case FOXAPI_OP_STORE_CANCEL: case FOXAPI_OP_STORE_UPDATE_ALL:
            case FOXAPP_OP_STORE_UPDATE: case FOXAPP_OP_STORE_REMOVE_BLOCKED: case FOXAPP_OP_STORE_ADD_ALL:
            case FOXAPP_OP_TELEMETRY_CONSENT: case FOXAPP_OP_SET_CATALOGUE_URL:
                return FOXAPI_GRANT_ADMIN;
            case FOXAPI_OP_PROBLEM_REPORT: case FOXAPI_OP_FEATURE_REQUEST: case FOXAPP_OP_NOTE_PANEL:
                return FOXAPI_GRANT_VIEW;
            default:
                if ((op & 0xFF00u) == 0x0A00u) { return FOXAPI_GRANT_TRANSMIT; }
                return FOXAPI_GRANT_SETTINGS;
        }
    }

    // Rule 2: validation and permission, answered at once on the caller's
    // thread against the published snapshot, delivered as a result.
    int32_t validate(const ApiSession& s, const FoxCommand& c, const FoxReceiverState* snap) const;

    enum class SessionFate { kOpen, kDetached, kClosed };
    SessionFate sessionFate(uint64_t id) {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) { return reaped_.count(id) != 0 ? SessionFate::kDetached : SessionFate::kClosed; }
        return it->second->detached.load() ? SessionFate::kDetached : SessionFate::kOpen;
    }

    void reapDetached(const std::vector<uint64_t>& ids) {
        for (uint64_t id : ids) {
            std::shared_ptr<ApiSession> gone;
            {
                std::lock_guard<std::mutex> lk(sessionsMutex_);
                auto it = sessions_.find(id);
                if (it != sessions_.end() && it->second->detached.load()) {
                    gone = it->second;
                    reaped_[id] = it->second;
                    sessions_.erase(it);
                }
            }
            if (gone) {
                std::lock_guard<std::mutex> lk(gone->m);
                gone->results.release();
                gone->events.release();
            }
        }
    }

    void refreshLive() {
        live_.clear();
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        for (auto& kv : sessions_) { live_.push_back(kv.second); }
    }

    void apply(const Pending& p);
    void applyTestHook(const Pending& p, FoxCommandResult& r, const std::string& key, const std::string& val);
    void deliverResult(uint64_t sessionId, int saferKind, const FoxCommandResult& r);
    void broadcast(FoxEvent e);
    static const char* refusalText(const Pending& p);

    static cascade::core::TxKeyTimers timersFrom(const Options& o) {
        cascade::core::TxKeyTimers t;
        t.pttHoldMs = o.pttHoldMs;
        t.latchTimeoutMs = o.latchTimeoutMs;
        t.keepaliveMs = o.keepaliveMs;
        t.remoteKeepaliveMs = o.remoteKeepaliveMs;
        return t.shortened();
    }

    // The sessions as the key rules see them: open, local or remote, and
    // when each last called heartbeat() (the web remote's internal one too).
    class SessionsView final : public cascade::core::TxKeyRules::Sessions {
    public:
        explicit SessionsView(ApiHostImpl& h) : h_(h) {}
        cascade::core::TxKeyRules::Liveness find(std::uint64_t id) const override {
            cascade::core::TxKeyRules::Liveness l;
            if (id == kWebSessionId) {
                l.present = true;
                l.remote = true;
                l.lastBeatMs = h_.webBeatMs_.load(std::memory_order_relaxed);
                return l;
            }
            std::lock_guard<std::mutex> lk(h_.sessionsMutex_);
            auto it = h_.sessions_.find(id);
            if (it == h_.sessions_.end() || it->second->detached.load()) { return l; }
            l.present = true;
            l.remote = (it->second->flags & FOXAPI_SESSION_REMOTE) != 0u;
            l.lastBeatMs = it->second->lastBeatMs.load(std::memory_order_relaxed);
            return l;
        }

    private:
        ApiHostImpl& h_;
    };

    void txPass();
    void publishNodeSpectra();
    std::string keyStateText() const;

    // The web remote's session id: never handed out by open_session (those
    // count up from 1), so it can never collide with one.
    static constexpr std::uint64_t kWebSessionId = ~std::uint64_t{0} - 1;

    Engine& engine_;
    Options opt_;
    cascade::core::TxKeyRules rules_;        // the pumping thread
    SessionsView sessionsView_;
    ApiSession webSession_;                  // its id and principal only
    std::atomic<std::int64_t> webBeatMs_{0};
    std::atomic<std::uint64_t> keyOwner_{0};  // rules_.keyOwner(), for heartbeat()
    bool lastPassFaulted_ = false;

    struct Outgoing {
        uint64_t sessionId;
        int saferKind;
        std::size_t order;  // application order, so the sort is stable without a buffer
        FoxCommandResult result;
    };
    std::vector<Outgoing> outbox_;                       // the pumping thread
    std::vector<std::shared_ptr<ApiSession>> live_;      // the pumping thread's scratch
    std::vector<Pending> work_;                          // the pumping thread's half of the queue
    std::vector<uint64_t> closedWork_;

    SpectrumRing spectrum_;
    cascade::core::SpectrumFrame frame_;                 // the pumping thread's copy, reused

    std::mutex sessionsMutex_;
    std::map<uint64_t, std::shared_ptr<ApiSession>> sessions_;
    std::map<uint64_t, std::shared_ptr<ApiSession>> reaped_;
    uint64_t nextSessionId_ = 0;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::vector<Pending> queue_;
    std::vector<uint64_t> closedSessions_;

    std::mutex tokensMutex_;
    std::map<std::string, std::int64_t> tokens_;
    int loginFailures_ = 0;
    std::int64_t lockedUntilMs_ = 0;

    std::atomic<bool> localMarked_{false};
    std::atomic<int> openDelayMs_{0};
    int stallAfterPassMs_ = 0;
    uint64_t submitted_ = 0;
    uint64_t gather_ = 0;
    uint64_t gatherFrom_ = 0;
    std::mutex keyStateMutex_;
    std::string keyState_;

    std::mutex listsMutex_;
    std::shared_ptr<const ApiListBlock> lists_;  // under listsMutex_

    // API 0.3: the patch radios' spectra, one ring per node with a radio
    // (made when a node's radio first publishes, dropped when it goes).
    std::mutex nodeSpectraMutex_;
    std::map<uint64_t, std::shared_ptr<SpectrumRing>> nodeSpectra_;
    std::map<uint64_t, uint64_t> nodeSpectraLast_;
    // API 0.3: the decoder-output backlog, a fixed ring.
    static constexpr std::size_t kDecoderLines = 500;
    struct DecoderLine {
        uint64_t number = 0;
        char plugin[96] = {};
        char text[160] = {};
    };
    std::mutex decoderLinesMutex_;
    std::array<DecoderLine, kDecoderLines> decoderLines_{};
    uint64_t decoderLinesNext_ = 0;          // under decoderLinesMutex_
    uint64_t decoderLinesSeen_ = 0;          // the pumping thread's
    // FOXAPI_EVENT_PATCH: the graph as the last pass saw it.
    uint64_t lastPatchHash_ = 0;
    uint64_t lastPatchEpoch_ = 0;
    bool lastPatchRunning_ = false;
    bool havePatch_ = false;

public:
    void setLists(std::shared_ptr<const ApiListBlock> l) {
        std::lock_guard<std::mutex> lk(listsMutex_);
        lists_ = std::move(l);
    }

private:

    // What the last pass's snapshot carried, for FOXAPI_EVENT_STATE.
    bool haveLastSeqs_ = false;
    std::uint64_t lastSeqs_[7] = {};
    bool lastFaulted_ = false;
};

int32_t ApiHostImpl::validate(const ApiSession& s, const FoxCommand& c, const FoxReceiverState* snap) const {
    if (!finite(c) || !textTerminated(c)) { return FOXAPI_BAD_ARGUMENT; }
    const bool remote = (s.flags & FOXAPI_SESSION_REMOTE) != 0u;
    const bool keyOp = c.op == FOXAPI_OP_TX_PTT || c.op == FOXAPI_OP_TX_LATCH || c.op == FOXAPI_OP_TX_REMOTE_ARM;
    if (c.op != FOXAPI_OP_SETTING_SET && !keyOp && !cascade::core::cmd::isKnownOp(c.op)) {
        return FOXAPI_UNSUPPORTED;  // a newer interface's op, or one not implemented: it degrades
    }
    if ((s.grants & grantFor(c.op)) == 0u) { return FOXAPI_DENIED; }
    if (c.op == FOXAPI_OP_SETTING_SET &&
        (!opt_.testHooks || std::strncmp(c.text, "test.", 5) != 0)) {
        // Only the test hooks, and only in an engine created with them; the
        // settings store is not served through the table yet (CAP_SETTINGS
        // is not set).
        return FOXAPI_UNSUPPORTED;
    }
    // The app's own extension ops are the standard window's vocabulary, and
    // some name paths on this machine: LOCAL sessions only.
    if (remote && cascade::core::cmd::isAppOp(c.op)) { return FOXAPI_DENIED; }
    // A REMOTE SESSION HAS THE WEB REMOTE'S POWER AND NO MORE: of the
    // transmitter it may only press and release the PTT, and pressing needs
    // the local operator's standing consent (TX_REMOTE_ARM, below).
    if (remote && grantFor(c.op) == FOXAPI_GRANT_TRANSMIT && c.op != FOXAPI_OP_TX_PTT) { return FOXAPI_DENIED; }
    const double v = c.num[0];
    switch (c.op) {
        case FOXAPI_OP_SET_CENTRE:
        case FOXAPI_OP_SET_FREQUENCY:
            return (v > 0.0 && v <= 6.0e9) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_VFO_OFFSET:
            return std::fabs(v) < 1.0e8 ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_STEP_TUNE:
            return (v > 0.0 && v <= 1.0e9 && c.ival[0] != 0 && std::llabs(c.ival[0]) <= 1000000) ? FOXAPI_OK
                                                                                                   : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_MODE:
            return (c.ival[0] >= FOXAPI_DEMOD_NFM && c.ival[0] <= FOXAPI_DEMOD_RAW) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_BANDWIDTH:
            return (v >= 100.0 && v <= 1.0e7) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_SQUELCH:
            return (v >= -200.0 && v <= 20.0) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_VOLUME:
            return (v >= 0.0 && v <= 1.0) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_DEEMPHASIS:
            return (c.ival[0] >= 0 && c.ival[0] <= 2) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_NR:
            return (c.ival[1] == 0 || (v >= 0.0 && v <= 1.0)) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_NOTCH:
            return (c.ival[1] == 0 || (v >= 10.0 && v <= 20000.0 && c.num[1] >= 0.1 && c.num[1] <= 1000.0))
                       ? FOXAPI_OK
                       : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_DISPLAY_RANGE:
            return (v >= -200.0 && v <= 40.0 && c.num[1] >= -200.0 && c.num[1] <= 40.0) ? FOXAPI_OK
                                                                                        : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_SAMPLE_RATE:
            return (v >= 8000.0 && v <= 61.44e6) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_GAIN:
            return (c.text[0] != '\0' && v >= -50.0 && v <= 200.0) ? FOXAPI_OK : FOXAPI_BAD_ARGUMENT;
        case FOXAPI_OP_BOOKMARK_ADD:
        case FOXAPI_OP_BOOKMARK_REMOVE_GROUP:
            return c.text[0] != '\0' ? FOXAPI_OK : FOXAPI_BAD_ARGUMENT;
        case FOXAPI_OP_SELECT_SOURCE:
            // No remote session names a path on this machine (API.md 5).
            // "file:" and "pluto:" in API.md; the engine's own spellings of
            // the same two ("open-pluto:") and the sound card's open, too.
            if (remote && (std::strncmp(c.text, "file:", 5) == 0 || std::strncmp(c.text, "pluto:", 6) == 0 ||
                           std::strncmp(c.text, "open-pluto:", 11) == 0 ||
                           std::strncmp(c.text, "soundcard:", 10) == 0)) {
                return FOXAPI_DENIED;
            }
            return FOXAPI_OK;
        case FOXAPI_OP_SETTING_SET:
            return std::strchr(c.text, '=') != nullptr ? FOXAPI_OK : FOXAPI_BAD_ARGUMENT;
        case FOXAPI_OP_SCAN_SERIAL_PORTS:
            // The ports of THIS machine (API 0.3): LOCAL sessions only.
            return remote ? FOXAPI_DENIED : FOXAPI_OK;
        case FOXAPI_OP_TX_SET_MODE:
            return (c.ival[0] >= 0 && c.ival[0] <= FOXAPI_TX_MODE_LSB) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_TX_SET_FREQUENCY:
            return (v > 0.0 && v <= 6.0e9) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_TX_SET_POWER:
            return (v <= 0.0 && v >= -89.75) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_TX_PTT:
            // THE KEY IS NOT SIMPLY QUEUED: a key request with no transmitter
            // behind it is refused now, so it cannot sit in the queue while
            // somebody opens a radio underneath it.
            if (c.ival[0] != 0 && (snap == nullptr || (snap->flags & FOXAPI_RX_TX_AVAILABLE) == 0u)) {
                return FOXAPI_NO_DEVICE;
            }
            if (c.ival[0] != 0 && remote && (snap == nullptr || (snap->flags & FOXAPI_RX_TX_REMOTE_ARMED) == 0u)) {
                return FOXAPI_DENIED;  // no local consent to remote transmit
            }
            return FOXAPI_OK;
        case FOXAPI_OP_TX_LATCH:
            if (c.ival[0] != 0) {
                // A latch keeps a radio keyed with nobody touching anything;
                // at the far end of a network that is the ordinary state.
                if ((s.flags & FOXAPI_SESSION_LOCAL) == 0u) { return FOXAPI_DENIED; }
                if (snap == nullptr || (snap->flags & FOXAPI_RX_TX_AVAILABLE) == 0u) { return FOXAPI_NO_DEVICE; }
            }
            return FOXAPI_OK;
        default:
            return FOXAPI_OK;
    }
}

const char* ApiHostImpl::refusalText(const Pending& p) {
    const bool remote = (p.sessionFlags & FOXAPI_SESSION_REMOTE) != 0u;
    switch (p.refused) {
        case FOXAPI_DENIED:
            if (remote && (p.cmd.op & 0xFF00u) == 0x0A00u) {
                return p.cmd.op == FOXAPI_OP_TX_PTT ? "the local operator has not given consent to remote transmit"
                                                    : "a remote session may only press and release the PTT";
            }
            if (remote && cascade::core::cmd::isAppOp(p.cmd.op)) {
                return "the application's own commands are for sessions on this machine";
            }
            if (remote && p.cmd.op == FOXAPI_OP_TX_LATCH) {
                return "the latch is a hands-on control and cannot be closed remotely";
            }
            if (remote && p.cmd.op == FOXAPI_OP_SELECT_SOURCE) {
                return "a remote session cannot name a file or an address on this machine";
            }
            return "this session does not hold the grant for that";
        case FOXAPI_OUT_OF_RANGE: return "a value is outside what the engine accepts";
        case FOXAPI_BAD_ARGUMENT: return "the command is malformed (NaN, unterminated or empty text)";
        case FOXAPI_UNSUPPORTED: return "this engine does not implement that command";
        case FOXAPI_NO_DEVICE: return "there is no transmitter open";
        default: return "refused";
    }
}

void ApiHostImpl::apply(const Pending& p) {
    const FoxCommand& c = p.cmd;
    FoxCommandResult r{};
    r.structSize = sizeof(r);
    r.ticket = p.ticket;
    r.op = c.op;
    r.status = FOXAPI_OK;
    auto finish = [&] { outbox_.push_back({p.sessionId, p.saferKind, outbox_.size(), r}); };
    if (p.refused != FOXAPI_OK) {
        r.status = p.refused;
        copyText(r.message, refusalText(p));
        finish();
        return;
    }
    const SessionFate fate = sessionFate(p.sessionId);
    if (fate != SessionFate::kOpen && !onlyMakesSafer(c)) {
        r.status = FOXAPI_DETACHED;
        copyText(r.message, fate == SessionFate::kDetached ? "the session was detached before this command was applied"
                                                           : "the session closed before this command was applied");
        finish();
        return;
    }
    // A radio command asked of a radio that has gone since it was submitted
    // is not applied to the one there now (the local queue's rule,
    // Engine::drainLocalCommands).
    if (p.sourceGen != engine_.sourceGen_.load() && isDeviceScopedOp(c.op)) {
        r.status = FOXAPI_NO_DEVICE;
        copyText(r.message, "the radio this was sent for has closed since");
        finish();
        return;
    }
    if (c.op == FOXAPI_OP_SETTING_SET) {
        const std::string kv = cascade::core::cmd::textOf(c, p.longText);
        const std::size_t eq = kv.find('=');
        applyTestHook(p, r, kv.substr(0, eq), kv.substr(eq + 1));
        finish();
        return;
    }
    if (c.op == FOXAPI_OP_TX_PTT || c.op == FOXAPI_OP_TX_LATCH || c.op == FOXAPI_OP_TX_REMOTE_ARM) {
        // THE KEY: decided by the rules, for THIS session, in its place in
        // the queue (core/tx_key_rules.hpp); the Transmitter follows at the
        // end of the pass (txPass).
        const bool remote = (p.sessionFlags & FOXAPI_SESSION_REMOTE) != 0u;
        const cascade::core::TxKeySender snd{p.sessionId, remote, p.principal, fate == SessionFate::kOpen};
        const bool on = c.ival[0] != 0;
        const cascade::core::TxKeyAnswer a =
            c.op == FOXAPI_OP_TX_REMOTE_ARM
                ? rules_.arm(snd, on, steadyMs())
                : rules_.key(snd, c.op == FOXAPI_OP_TX_LATCH, on, engine_.transmitter_.haveSink(), steadyMs());
        r.status = a.status;
        r.applied[0] = a.applied0;
        copyText(r.message, a.message.c_str());
        if (a.status == FOXAPI_OK && on && c.op != FOXAPI_OP_TX_REMOTE_ARM) {
            // A key granted: the TX thread's front-end stamp is the holder's
            // LAST HEARTBEAT (a grant, a PTT re-assertion included, is never
            // a heartbeat of its own - API 0.2), and a remote PTT buys the
            // Transmitter's own hold.
            if (remote) {
                engine_.transmitter_.keyRemote();
            } else {
                engine_.transmitter_.frontEndAliveAt(sessionsView_.find(p.sessionId).lastBeatMs);
            }
            keyOwner_.store(rules_.keyOwner(), std::memory_order_relaxed);
        }
        finish();
        return;
    }
    const FoxCommandResult applied = engine_.applyCommand(c, p.longText);
    r.status = applied.status;
    r.flags = applied.flags;
    r.applied[0] = applied.applied[0];
    r.applied[1] = applied.applied[1];
    std::memcpy(r.message, applied.message, sizeof(r.message));
    r.message[sizeof(r.message) - 1] = '\0';
    finish();
}

void ApiHostImpl::applyTestHook(const Pending& p, FoxCommandResult& r, const std::string& key,
                                const std::string& val) {
    (void)p;
    if (key == "test.stall") {
        // The control pass is busy this long (at most a second), so a test
        // can queue commands and change a session behind them first.
        std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(std::atoi(val.c_str()), 0, 1000)));
    } else if (key == "test.stallAfterPass") {
        stallAfterPassMs_ = std::clamp(std::atoi(val.c_str()), 0, 1000);
    } else if (key == "test.openDelay") {
        openDelayMs_.store(std::clamp(std::atoi(val.c_str()), 0, 1000));
    } else if (key == "test.gather") {
        std::lock_guard<std::mutex> lk(queueMutex_);
        gatherFrom_ = submitted_;
        gather_ = static_cast<uint64_t>(std::clamp(std::atoi(val.c_str()), 1, 64));
    } else if (key == "test.resetKey") {
        // Test seam: the key rules as a fresh engine has them (no key, no
        // latch, no mark, no consent), so one engine can run many cases that
        // must each start clean (test_engine_api_merge).
        rules_ = cascade::core::TxKeyRules(timersFrom(opt_));
        keyOwner_.store(0);
        lastPassFaulted_ = false;
    } else if (key == "test.fault") {
        // A worker "dies": the receiver stops and the key opens, exactly as a
        // USB radio pulled mid-stream does (the fault latches until RUN 1).
        engine_.testFault_ = val;
        if (!val.empty()) {
            rules_.endKey("a fault stopped the engine", steadyMs());
            // The receiver stops by the one control path (RUN 0), as every
            // stop does; the key already ended above, with the fault's reason.
            (void)engine_.applyCommand(cascade::core::cmd::makeInt(FOXAPI_OP_RUN, 0));
        }
    } else if (key == "test.decoderLine") {
        // Stands in for a decoder's output: "plugin|text", as
        // pumpDecoderOutput appends it.
        const std::size_t bar = val.find('|');
        engine_.decoderLog_.push_back({val.substr(0, bar), bar == std::string::npos ? std::string() : val.substr(bar + 1)});
        while (engine_.decoderLog_.size() > Engine::kDecoderLogMax) { engine_.decoderLog_.pop_front(); }
        ++engine_.decoderLinesTotal_;
    } else if (key == "test.muteBy") {
        // Stands in for decoder plugins that mute the audio on their preset:
        // "name@hz;name@hz" - each a running, muting plugin with one preset at
        // that frequency (so tuning there mutes, by that name, in the next
        // pass). An empty value takes them all away: with none running, the
        // latch the pass keeps after a tune-away lets go.
        engine_.muteStates_.clear();
        std::size_t a = 0;
        while (a < val.size()) {
            const std::size_t semi = val.find(';', a);
            const std::string one = val.substr(a, semi == std::string::npos ? std::string::npos : semi - a);
            const std::size_t at = one.find('@');
            if (at != std::string::npos) {
                cascade::core::MutePlugin m;
                m.name = one.substr(0, at);
                m.key = m.name + ".dll";  // a module's file name, as pluginKey gives one: not its name
                m.running = true;
                m.mutes = true;
                cascade::core::MutePreset ps;
                ps.frequencyHz = std::atof(one.c_str() + at + 1);
                ps.label = m.name;
                m.presets.push_back(std::move(ps));
                engine_.muteStates_.push_back(std::move(m));
            }
            if (semi == std::string::npos) { break; }
            a = semi + 1;
        }
    } else if (key == "test.audioPlaying") {
        // Stands in for an audio-out plugin holding the speakers, or handing
        // them back (empty): "name|gaps|gapFrames", the counters optional.
        // PluginRunner::setPlayingForTest.
        const std::size_t b1 = val.find('|');
        const std::size_t b2 = b1 == std::string::npos ? b1 : val.find('|', b1 + 1);
        const std::uint64_t gaps = b1 == std::string::npos ? 0 : std::strtoull(val.c_str() + b1 + 1, nullptr, 10);
        const std::uint64_t gapFrames = b2 == std::string::npos ? 0 : std::strtoull(val.c_str() + b2 + 1, nullptr, 10);
        engine_.pluginRunner_.setPlayingForTest(val.substr(0, b1), gaps, gapFrames);
    } else if (key == "test.audioClose") {
        // The output stream dies under a healthy receiver (a USB sound card
        // re-enumerating - the case the audio watchdog exists for): the stream
        // is closed from under the engine, which notices within its second and
        // reopens it. Never while a worker is inside the device's open.
        if (!engine_.audioOpen_.inFlight()) { engine_.pipeline_.audio().close(); }
    } else if (key == "test.biasStandIn") {
        // Stands in for FOXSDR_FORCE_BIAS_KEY (a bounded run's census seam): "accept" or
        // "refuse" puts the stand-in bias tee in front of the key (while no real radio with
        // one is open) and "none" takes it away; either way it starts off. API 0.3 8.3.
        engine_.biasStandIn_ = val == "accept"   ? cascade::gui::BiasStandIn::Accept
                               : val == "refuse" ? cascade::gui::BiasStandIn::Refuse
                                                 : cascade::gui::BiasStandIn::None;
        engine_.biasStandInOn_ = false;
    } else if (key == "test.gainUnit") {
        // The open radio's gain stages read in this unit: "step" (the
        // hardware's own steps, an Airspy's) or "dB". The fake radio has only
        // decibels.
        for (cascade::source::GainInfo& g : engine_.deviceGainRanges_) {
            g.unit = val == "step" ? cascade::source::GainUnit::Steps : cascade::source::GainUnit::Decibels;
        }
    } else if (key == "test.statusLine") {
        // Stands in for whatever writes a status line: "key|text".
        const std::size_t bar = val.find('|');
        const std::string k = val.substr(0, bar);
        const std::string t = bar == std::string::npos ? std::string() : val.substr(bar + 1);
        if (k == "presetNote") { engine_.presetNote_ = t; }
        if (k == "recordNotice") { engine_.recordNotice_ = t; }
        if (k == "transmitError") { engine_.transmitError_ = t; }
    } else if (key == "test.catalogue") {
        // Stands in for a fetched catalogue entry: "id|name|legal notice".
        cascade::core::PluginCatalogEntry e;
        const std::size_t a = val.find('|');
        const std::size_t b = a == std::string::npos ? a : val.find('|', a + 1);
        e.id = val.substr(0, a);
        e.name = a == std::string::npos ? std::string() : val.substr(a + 1, b - a - 1);
        e.legalNotice = b == std::string::npos ? std::string() : val.substr(b + 1);
        e.version = "1.0.0";
        engine_.catalog_.push_back(std::move(e));
    } else if (key == "test.bookmark") {
        // Stands in for an import: "name|hz|mode|bw|group", mode a
        // FOXAPI_DEMOD_* - a grouped bookmark, as an imported list makes.
        std::vector<std::string> f;
        std::size_t a = 0;
        while (true) {
            const std::size_t bar = val.find('|', a);
            f.push_back(val.substr(a, bar == std::string::npos ? std::string::npos : bar - a));
            if (bar == std::string::npos) { break; }
            a = bar + 1;
        }
        const int demod = f.size() == 5 ? std::atoi(f[2].c_str()) : 0;
        if (f.size() != 5 || demod < 1 || demod > 8) {
            r.status = FOXAPI_BAD_ARGUMENT;
            copyText(r.message, "test.bookmark is name|hz|mode|bw|group");
            return;
        }
        cascade::core::Bookmark b;
        b.name = f[0];
        b.freqHz = std::atof(f[1].c_str());
        b.mode = cascade::net::kDemodNames[demod];
        b.bandwidthHz = std::atof(f[3].c_str());
        b.group = f[4];
        engine_.freqMgr_.add(b);
        engine_.saveBookmarks();
    } else {
        r.status = FOXAPI_NOT_FOUND;
        copyText(r.message, "no such test hook");
    }
}

void ApiHostImpl::deliverResult(uint64_t sessionId, int saferKind, const FoxCommandResult& r) {
    std::shared_ptr<ApiSession> s;
    {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) { return; }  // closed meanwhile: nobody to tell
        s = it->second;
    }
    FoxCommandResult out = r;
    if (out.status != FOXAPI_OK) {
        out.flags |= FOXAPI_RESULT_REFUSED;
    } else {
        out.flags &= ~FOXAPI_RESULT_REFUSED;
    }
    std::lock_guard<std::mutex> lk(s->m);
    if (saferKind < 0) {
        // Bounded by FOXAPI_MAX_PENDING: the command counts until polled.
        if (!s->results.full()) { s->results.push_back(out); }
        return;
    }
    ApiSession::SaferSlot& slot = s->safer[static_cast<std::size_t>(saferKind)];
    if (slot.ticket != r.ticket) { return; }
    if (slot.copies > 0) { --slot.copies; }
    if (slot.read) {
        if (slot.copies == 0) { slot = ApiSession::SaferSlot{}; }
        return;
    }
    if (slot.inResults) {
        for (std::size_t i = 0; i < s->results.size(); ++i) {
            if (s->results.at(i).ticket == r.ticket) {
                s->results.at(i) = out;
                break;
            }
        }
    } else {
        if (!s->results.full()) { s->results.push_back(out); }
        slot.inResults = true;
    }
}

void ApiHostImpl::broadcast(FoxEvent e) {
    e.structSize = sizeof(e);
    e.unixMs = unixMs();
    for (auto& s : live_) {
        std::lock_guard<std::mutex> lk(s->m);
        if ((s->eventMask & FOXAPI_EVENT_MASK(e.kind)) == 0u) { continue; }
        if (s->events.full()) {
            s->events.pop_front();
            ++s->eventsDropped;
        }
        e.seq = ++s->eventSeq;
        s->events.push_back(e);
    }
}

void ApiHostImpl::beginPass() {
    work_.clear();
    closedWork_.clear();
    {
        std::unique_lock<std::mutex> lk(queueMutex_);
        if (gather_ > 0) {
            // Test seam (test.gather): this pass takes the queue only once
            // `gather_` more commands have been submitted (at most 2 s).
            const uint64_t want = gatherFrom_ + gather_;
            queueCv_.wait_for(lk, std::chrono::seconds(2), [this, want] { return submitted_ >= want; });
            gather_ = 0;
        }
        work_.swap(queue_);
        closedWork_.swap(closedSessions_);
    }
    refreshLive();
    // Sessions closed or detached since the last pass are dealt with FIRST,
    // before any command is applied (the reference engine's round-3 rule):
    // any key or consent they held goes now, nothing they queued can act for
    // them, and only the safer commands they left are applied.
    for (uint64_t id : closedWork_) {
        rules_.sessionGone(id, sessionFate(id) == SessionFate::kDetached, steadyMs());
    }
    for (const Pending& p : work_) { apply(p); }
    reapDetached(closedWork_);
    expireTokens();
    txPass();
    if (opt_.testHooks) {
        const std::string ks = keyStateText();
        std::lock_guard<std::mutex> lk(keyStateMutex_);
        keyState_ = ks;
    }
}

// Once a pass, after the commands: the rules' clocks (the hold, the latch's
// deadline, every key holder's and consenter's keep-alive), then the
// Transmitter driven from what they decided and ticked - the control stamp
// its TX thread watches - and, if the Transmitter let go by itself (a fault,
// its own dead-man's handle, a radio that would not key), the rules told.
void ApiHostImpl::txPass() {
    const std::int64_t now = steadyMs();
    cascade::core::Transmitter& tx = engine_.transmitter_;
    const bool faulted = engine_.pipeline_.faulted() || !engine_.testFault_.empty();
    if (faulted && !lastPassFaulted_) { rules_.endKey("a fault stopped the engine", now); }
    lastPassFaulted_ = faulted;
    rules_.tick(now, sessionsView_);
    // Drive: the latch and a local PTT's hold are levels; a remote PTT's hold
    // was bought when it was applied (keyRemote) and is dropped here the
    // moment the rules end it.
    const bool keyed = rules_.keyed();
    const bool remoteOwner = keyed && rules_.ownerRemote();
    tx.setLatched(rules_.latched());
    tx.setPttHeld(keyed && !remoteOwner && rules_.holding());
    if (!(remoteOwner && rules_.holding())) {
        tx.releaseRemote(rules_.unkeyReason().empty() ? "the remote let go" : rules_.unkeyReason().c_str());
    }
    tx.tick();
    if (rules_.keyed() && !tx.pttHeld() && !tx.latched() && !tx.remoteKeyed()) {
        std::string why = tx.lastAutoUnkeyReason();
        if (why.empty()) {
            const std::string err = tx.lastError();
            why = err.empty() ? std::string("the transmitter stopped on its own") : "the radio would not key: " + err;
        }
        rules_.endKey(why, now);
    }
    keyOwner_.store(rules_.keyOwner(), std::memory_order_relaxed);
    for (const std::string& why : rules_.takeUnkeyEvents()) {
        FoxEvent e{};
        e.kind = FOXAPI_EVENT_TX_UNKEYED;
        copyText(e.text, why.c_str());
        broadcast(e);
    }
    // FOXAPI_RX_TX_LATCH_RELEASE_FIRST, per session (it may trail the
    // snapshot by this pass).
    for (const auto& s : live_) { s->releaseFirst.store(rules_.releaseFirst(s->principal, s->id)); }
    localMarked_.store(rules_.marked("local"), std::memory_order_relaxed);
}

std::string ApiHostImpl::keyStateText() const {
    return rules_.stateText() + " running=" + std::to_string(engine_.pipeline_.running() ? 1 : 0) +
           " tx=" + std::to_string(engine_.transmitter_.haveSink() ? 1 : 0);
}

void ApiHostImpl::publishNodeSpectra() {
    const auto& spectra = engine_.patchSpectra_;
    bool changedSet = spectra.size() != nodeSpectraLast_.size();
    if (!changedSet) {
        for (const auto& e : spectra) {
            if (nodeSpectraLast_.count(e.first) == 0) {
                changedSet = true;
                break;
            }
        }
    }
    if (changedSet) {
        std::lock_guard<std::mutex> lk(nodeSpectraMutex_);
        for (auto it = nodeSpectra_.begin(); it != nodeSpectra_.end();) {
            it = spectra.count(it->first) == 0 ? nodeSpectra_.erase(it) : std::next(it);
        }
        for (auto it = nodeSpectraLast_.begin(); it != nodeSpectraLast_.end();) {
            it = spectra.count(it->first) == 0 ? nodeSpectraLast_.erase(it) : std::next(it);
        }
        for (const auto& e : spectra) {
            if (nodeSpectra_.count(e.first) == 0) {
                nodeSpectra_[e.first] = std::make_shared<SpectrumRing>();
                nodeSpectraLast_[e.first] = 0;
            }
        }
    }
    for (const auto& e : spectra) {
        uint64_t& last = nodeSpectraLast_[e.first];
        if (e.second.seq == last || e.second.db.empty()) { continue; }
        last = e.second.seq;
        std::shared_ptr<SpectrumRing> ring;
        {
            std::lock_guard<std::mutex> lk(nodeSpectraMutex_);
            ring = nodeSpectra_[e.first];
        }
        double centre = 0.0, span = 0.0;
        const auto r = engine_.patchRadios_.find(e.first);
        if (r != engine_.patchRadios_.end()) {
            centre = r->second->centreHz();
            span = r->second->rateHz();
        }
        ring->publish(e.second.db.data(), e.second.db.size(), centre, span, unixMs());
    }
}

void ApiHostImpl::endPass() {
    // THE SPECTRUM, copied from the pipeline's newest frame into the ring
    // interfaces read without a lock (one lock of the pipeline's frame slot a
    // pass - the lock the window took once a frame before stage 4).
    if (engine_.pipeline_.getLatestFrame(frame_) && !frame_.dbBins.empty()) {
        cascade::source::IqSource& src = engine_.pipeline_.activeSource();
        spectrum_.publish(frame_.dbBins.data(), frame_.dbBins.size(), src.centerFrequencyHz(), src.sampleRateHz(),
                          unixMs());
    }
    // API 0.3: THE PATCH RADIOS' SPECTRA, each into its own ring (a ring is
    // made only when a node's radio first appears, and dropped with it).
    publishNodeSpectra();
    // Results go out only now, AFTER the state they describe has been
    // published, and in TICKET order per session (a merged safer command is
    // applied in the place of its LAST send). std::sort on (session, ticket,
    // order) is stable by construction and allocates nothing.
    std::sort(outbox_.begin(), outbox_.end(), [](const Outgoing& x, const Outgoing& y) {
        if (x.sessionId != y.sessionId) { return x.sessionId < y.sessionId; }
        if (x.result.ticket != y.result.ticket) { return x.result.ticket < y.result.ticket; }
        return x.order < y.order;
    });
    for (const Outgoing& o : outbox_) { deliverResult(o.sessionId, o.saferKind, o.result); }
    outbox_.clear();
    // Events: which groups the pass's publish moved, and a fault.
    cascade::core::PublishedState ps;
    if (engine_.receiverSnapshot_->read(ps)) {
        const std::uint64_t seqs[7] = {ps.rx.tuneSeq, ps.rx.modeSeq, ps.rx.deviceSeq, ps.rx.audioSeq,
                                       ps.rx.displaySeq, ps.rx.txSeq, ps.rx.listSeq};
        if (haveLastSeqs_) {
            uint32_t moved = 0;
            for (int i = 0; i < 7; ++i) {
                if (seqs[i] != lastSeqs_[i]) { moved |= 1u << i; }
            }
            if (moved != 0u) {
                FoxEvent e{};
                e.kind = FOXAPI_EVENT_STATE;
                e.value[0] = moved;
                broadcast(e);
            }
        }
        std::copy(seqs, seqs + 7, lastSeqs_);
        haveLastSeqs_ = true;
        const bool faulted = (ps.rx.flags & FOXAPI_RX_FAULTED) != 0u;
        if (faulted && !lastFaulted_) {
            FoxEvent e{};
            e.kind = FOXAPI_EVENT_FAULT;
            copyText(e.text, ps.rx.faultMessage);
            broadcast(e);
        }
        lastFaulted_ = faulted;
    }
    // API 0.3: FOXAPI_EVENT_PATCH - the graph, its epoch or whether it runs
    // moved in this pass (from the list block the pass published).
    if (const ApiListBlock* b = engine_.apiLists_.get()) {
        if (havePatch_ && (b->patchGraphHash != lastPatchHash_ || b->patchEpoch != lastPatchEpoch_ ||
                           b->patchRunning != lastPatchRunning_)) {
            FoxEvent e{};
            e.kind = FOXAPI_EVENT_PATCH;
            e.value[0] = static_cast<double>(b->patchEpoch);
            e.value[1] = b->patchRunning ? 1.0 : 0.0;
            broadcast(e);
        }
        havePatch_ = true;
        lastPatchHash_ = b->patchGraphHash;
        lastPatchEpoch_ = b->patchEpoch;
        lastPatchRunning_ = b->patchRunning;
    }
    if (stallAfterPassMs_ > 0) {  // test seam: test.stallAfterPass
        const int ms = stallAfterPassMs_;
        stallAfterPassMs_ = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

int32_t ApiHostImpl::readList(const ApiSession* s, uint32_t list, uint32_t index, FoxListItem* out) {
    if (out != nullptr && out->structSize < FOXAPI_MIN_LIST_ITEM) { return FOXAPI_BAD_ARGUMENT; }
    const bool local = (s->flags & FOXAPI_SESSION_LOCAL) != 0u;
    std::shared_ptr<const ApiListBlock> b;
    {
        std::lock_guard<std::mutex> lk(listsMutex_);
        b = lists_;
    }
    if (b == nullptr) { b = std::make_shared<const ApiListBlock>(); }
    FoxListItem it{};
    it.structSize = sizeof(it);
    it.list = list;
    it.index = index;
    int32_t count = 0;
    const auto want = [&](std::size_t n) {
        count = static_cast<int32_t>(n);
        return static_cast<std::size_t>(index) < n;
    };
    switch (list) {
        case FOXAPI_LIST_MODES:
            // value[0]: FOXAPI_DEMOD_*; value[1]: the mode's default bandwidth.
            if (want(8)) {
                it.id = cascade::gui::abiDemodForModeIndex(static_cast<int>(index));
                it.value[0] = static_cast<double>(it.id);
                it.value[1] = cascade::gui::kBwHz[cascade::gui::kModeDefaultBw[index]];
                copyText(it.name, cascade::gui::kModeNames[index]);
                if (static_cast<uint32_t>(it.id) == b->mode) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_BANDWIDTHS:
            if (want(static_cast<std::size_t>(cascade::gui::kBwCount))) {
                it.value[0] = cascade::gui::kBwHz[index];
                char label[32];
                cascade::gui::formatBandwidth(it.value[0], label, sizeof(label));
                copyText(it.name, label);
                if (it.value[0] == b->bandwidthHz) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_DEVICES:
            if (want(b->devices.size())) {
                const auto& d = b->devices[index];
                it.id = index + 1u;
                copyText(it.name, d.label.c_str());
                copyText(it.detail, d.id.c_str());
                if (d.active) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_SAMPLE_RATES:
            if (want(b->rates.size())) {
                it.value[0] = b->rates[index];
                if (it.value[0] == b->rateHz) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_GAINS:
            if (want(b->gains.size())) {
                const auto& g = b->gains[index];
                copyText(it.name, g.name.c_str());
                copyText(it.detail, g.unit);  // 0.3: "dB" or "step" (docs/api-0.3-additions.md 5.1)
                it.value[0] = g.minDb;
                it.value[1] = g.maxDb;
                it.value[2] = g.stepDb;
                it.value[3] = g.currentDb;
                if (g.agc) { it.flags |= FOXAPI_ITEM_AGC; }  // 0.3 (8.3): its own AGC is driving it
            }
            break;
        case FOXAPI_LIST_ANTENNAS:
            if (want(b->antennas.size())) {
                copyText(it.name, b->antennas[index].c_str());
                if (b->antennas[index] == b->antenna) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_BOOKMARKS: {
            // The default block (no publish yet, or a rebuilt host) has no
            // bookmarks pointer: test it BEFORE dereferencing (a reference
            // bound through a null shared_ptr is undefined behaviour, and
            // aborts a standard library built with assertions).
            if (want(b->bookmarks ? b->bookmarks->size() : 0)) {
                const cascade::core::Bookmark& bm = (*b->bookmarks)[index];
                it.id = bm.id;
                copyText(it.name, bm.name.c_str());
                copyText(it.detail, bm.group.c_str());
                it.value[0] = bm.freqHz;
                it.value[1] = static_cast<double>(cascade::core::cmd::foxDemodFromName(bm.mode.c_str()));
                it.value[2] = bm.bandwidthHz;
                if (bm.favourite) { it.flags |= FOXAPI_ITEM_FAVOURITE; }
            }
            break;
        }
        case FOXAPI_LIST_BOOKMARK_GROUPS: {
            // Named groups, sorted by name, with their counts (the bookmarks
            // are sorted by frequency, so the groups are gathered here).
            std::map<std::string, int> groups;
            if (b->bookmarks) {
                for (const cascade::core::Bookmark& bm : *b->bookmarks) {
                    if (!bm.group.empty()) { ++groups[bm.group]; }
                }
            }
            if (want(groups.size())) {
                auto g = std::next(groups.begin(), static_cast<std::ptrdiff_t>(index));
                copyText(it.name, g->first.c_str());
                it.value[0] = g->second;
            }
            break;
        }
        case FOXAPI_LIST_AUDIO_DEVICES:
            if (want(b->audioDevices.size())) {
                const auto& a = b->audioDevices[index];
                it.id = index + 1u;
                it.value[0] = static_cast<double>(a.index);
                copyText(it.name, a.name.c_str());
                copyText(it.detail, std::to_string(a.index).c_str());
                if (a.active) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_PLUGINS:
            if (want(b->plugins.size())) {
                const auto& p = b->plugins[index];
                it.id = index + 1u;
                copyText(it.name, p.name.empty() ? p.fileName.c_str() : p.name.c_str());
                copyText(it.detail, p.fileName.c_str());
                if (p.loaded) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        // ---- the 0.2 lists step 1 left out ----------------------------------
        case FOXAPI_LIST_DECODERS:
            if (want(b->decoders.size())) {
                const auto& d = b->decoders[index];
                it.id = index + 1u;
                copyText(it.name, d.name.c_str());
                copyText(it.detail, d.status.c_str());
                if (d.running) { it.flags |= FOXAPI_ITEM_ACTIVE; }
                if (d.fed) { it.flags |= FOXAPI_ITEM_FED; }
            }
            break;
        case FOXAPI_LIST_CATALOGUE:
            if (want(b->store.size())) {
                const auto& e = b->store[index];
                it.id = index + 1u;
                copyText(it.name, e.name.c_str());
                copyText(it.detail, e.id.c_str());
                if (e.installed) { it.flags |= FOXAPI_ITEM_INSTALLED; }
                if (e.compatible) { it.flags |= FOXAPI_ITEM_COMPATIBLE; }
            }
            break;
        case FOXAPI_LIST_TX_DEVICES:
            // The transmitter this engine opens is a Pluto by address: ONE
            // entry, the address TX_OPEN will be given.
            if (want(1)) {
                it.id = 1;
                copyText(it.name, "ADALM-Pluto");
                copyText(it.detail, b->txArgs.c_str());
                if (b->txOpen) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_MARKERS:
            if (want(b->markers.size())) {
                const auto& m = b->markers[index];
                it.id = m.id;
                copyText(it.name, m.label.c_str());
                copyText(it.detail, m.plugin.c_str());
                it.value[0] = m.freqHz;
                it.value[1] = m.widthHz;
                it.value[2] = m.colourRgba;
            }
            break;
        case FOXAPI_LIST_BAND_PLAN:
            if (want(b->bandPlan.size())) {
                const auto& e = b->bandPlan[index];
                copyText(it.name, e.name.c_str());
                copyText(it.detail, e.service.c_str());
                it.value[0] = e.startHz;
                it.value[1] = e.endHz;
                it.value[2] = e.colourRgba;
            }
            break;
        case FOXAPI_LIST_PRESETS:
            if (want(b->presets ? b->presets->size() : 0)) {
                const auto& pr = (*b->presets)[index];
                it.id = pr.index;
                copyText(it.name, pr.label.c_str());
                copyText(it.detail, pr.plugin.c_str());
                it.value[0] = pr.freqHz;
                it.value[1] = pr.demodMode;
                it.value[2] = pr.bandwidthHz;
            }
            break;
        case FOXAPI_LIST_USER_PRESETS:
            if (want(b->userPresets.size())) {
                const auto& pr = b->userPresets[index];
                it.id = pr.index + 1u;
                copyText(it.name, pr.label.c_str());
                copyText(it.detail, pr.plugin.c_str());
                it.value[0] = pr.freqHz;
                it.value[1] = pr.demodMode;
                it.value[2] = pr.bandwidthHz;
            }
            break;
        case FOXAPI_LIST_DEVICE_OPTIONS:
            if (want(b->deviceOptions.size())) {
                copyText(it.name, b->deviceOptions[index].first.c_str());
                if (b->deviceOptions[index].second) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_SERIAL_PORTS:
            if (want(b->serialPorts.size())) { copyText(it.name, b->serialPorts[index].c_str()); }
            break;
        // ---- API 0.3 (docs/api-0.3-additions.md) ------------------------------
        case FOXAPI_LIST_STATUS_LINES: {
            std::size_t n = 0;
            for (const auto& l : b->statusLines) { n += l.text.empty() ? 0 : 1; }
            if (want(n)) {
                std::size_t k = 0;
                for (const auto& l : b->statusLines) {
                    if (l.text.empty()) { continue; }
                    if (k++ == index) {
                        copyText(it.name, l.key);
                        copyText(it.detail, l.text.c_str());
                        it.value[0] = l.level;
                        break;
                    }
                }
            }
            break;
        }
        case FOXAPI_LIST_CONVERTERS:
            if (want(b->converters.size())) {
                const auto& c = b->converters[index];
                // A radio key names a device on this machine: LOCAL only.
                if (local) { copyText(it.detail, c.radioKey.c_str()); }
                it.value[0] = c.mode;
                it.value[1] = c.loHz;
                it.value[2] = c.inverted ? 1.0 : 0.0;
                // 0.3 (8.3), the open radio's entry: the radio's own centre
                // readback, LIVE from the pass's atomic (a retune rebuilds no block).
                if (c.radioCentre != nullptr) { it.value[3] = c.radioCentre->load(std::memory_order_relaxed); }
                if (c.active) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        case FOXAPI_LIST_GAIN_MODES:
        case FOXAPI_LIST_DECIMATIONS: {
            const auto& v = list == FOXAPI_LIST_GAIN_MODES ? b->gainModes : b->decimations;
            if (want(v.size())) {
                copyText(it.name, v[index].name.c_str());
                it.value[0] = v[index].value;
                it.value[1] = v[index].value2;
                if (v[index].active) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        }
        case FOXAPI_LIST_PATCH_NODES:
            if (want(b->patchNodes.size())) {
                const auto& n = b->patchNodes[index];
                it.id = n.id;
                copyText(it.name, n.name.c_str());
                copyText(it.detail, n.kind);
                it.value[0] = n.x;
                it.value[1] = n.y;
                it.value[2] = n.freqHz;
                it.value[3] = n.rateHz;
                if (n.on) { it.flags |= FOXAPI_ITEM_ACTIVE; }
                if (n.refused) { it.flags |= FOXAPI_ITEM_REFUSED; }
                if (n.fault) { it.flags |= FOXAPI_ITEM_FAULT; }
            }
            break;
        case FOXAPI_LIST_PATCH_WIRES:
            if (want(b->patchWires.size())) {
                const auto& w = b->patchWires[index];
                it.id = index + 1u;
                it.value[0] = static_cast<double>(w.from);
                it.value[1] = static_cast<double>(w.fromPort);
                it.value[2] = static_cast<double>(w.to);
                it.value[3] = static_cast<double>(w.toPort);
            }
            break;
        case FOXAPI_LIST_PATCH_PROBLEMS:
            if (want(b->patchProblems.size())) {
                const auto& pr = b->patchProblems[index];
                it.id = pr.node;
                copyText(it.name, pr.problem.c_str());
                copyText(it.detail, pr.text.c_str());
            }
            break;
        case FOXAPI_LIST_PATCH_PARTS:
            if (want(b->patchParts.size())) {
                const auto& pp = b->patchParts[index];
                it.id = index + 1u;
                copyText(it.name, pp.name.c_str());
                copyText(it.detail, pp.key.c_str());
                it.value[0] = pp.feed;
                it.value[1] = pp.rateHz;
            }
            break;
        case FOXAPI_LIST_PATCH_RADIOS:
            if (want(b->patchRadios.size())) {
                const auto& r = b->patchRadios[index];
                it.id = r.node;
                copyText(it.name, r.device.c_str());
                copyText(it.detail, r.error.c_str());
                it.value[0] = r.rateHz;
                if (r.open) { it.flags |= FOXAPI_ITEM_ACTIVE; }
                if (r.fault) { it.flags |= FOXAPI_ITEM_FAULT; }
            }
            break;
        case FOXAPI_LIST_RECORDINGS:
            if (want(b->recordings.size())) {
                const auto& r = b->recordings[index];
                it.id = index + 1u;
                copyText(it.name, r.name.c_str());
                if (local) { copyText(it.detail, r.path.c_str()); }  // a path on this machine
                it.value[0] = r.kind;
                it.value[2] = r.rateHz;
                if (const auto* rec = static_cast<const cascade::core::Recorder*>(r.recorder)) {
                    // LIVE: the recorder's own atomics, no rebuild per block.
                    it.value[1] = static_cast<double>(rec->bytesWritten());
                    // ...and the take's length from the recorder's own steady
                    // stamps (0.3): live too, so a take that is growing moves
                    // neither the block nor listSeq.
                    it.value[3] = rec->elapsedSeconds();
                    if (rec->recording()) { it.flags |= FOXAPI_ITEM_ACTIVE; }
                    if (rec->sizeLimitReached()) {
                        it.flags |= FOXAPI_ITEM_FULL;
                    } else if (rec->writeFailed()) {
                        it.flags |= FOXAPI_ITEM_STALLED;
                    }
                } else if (r.active) {
                    it.flags |= FOXAPI_ITEM_ACTIVE;
                }
            }
            break;
        case FOXAPI_LIST_DECODER_LINES: {
            std::lock_guard<std::mutex> lk(decoderLinesMutex_);
            const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(decoderLinesNext_, kDecoderLines));
            if (want(n)) {
                const DecoderLine& d = decoderLines_[(decoderLinesNext_ - n + index) % kDecoderLines];
                it.id = d.number;
                copyText(it.name, d.plugin);
                copyText(it.detail, d.text);
            }
            break;
        }
        case FOXAPI_LIST_STORE_BLOCKED:
        case FOXAPI_LIST_STORE_UPDATES: {
            const auto& v = list == FOXAPI_LIST_STORE_BLOCKED ? b->storeBlocked : b->storeUpdates;
            if (want(v.size())) {
                it.id = index + 1u;
                copyText(it.name, v[index].first.c_str());
                copyText(it.detail, v[index].second.c_str());
            }
            break;
        }
        case FOXAPI_LIST_BAND_PLANS:
            if (want(b->bandPlans.size())) {
                const auto& pl = b->bandPlans[index];
                it.id = index + 1u;
                copyText(it.name, pl.name.c_str());
                copyText(it.detail, pl.id.c_str());
                if (pl.active) { it.flags |= FOXAPI_ITEM_ACTIVE; }
            }
            break;
        default:
            return FOXAPI_UNSUPPORTED;
    }
    if (out != nullptr && static_cast<int32_t>(index) < count) { copyOut(it, out, FOXAPI_MIN_LIST_ITEM); }
    return count;
}

// ---------------------------------------------------------------------------
// ApiHost
// ---------------------------------------------------------------------------

ApiHost::ApiHost(Engine& engine, const std::string& options)
    : impl_(std::make_unique<ApiHostImpl>(engine, options)) {}

ApiHost::~ApiHost() = default;

FoxEngine* ApiHost::handle() { return impl_.get(); }

void ApiHost::beginPass() { impl_->beginPass(); }
void ApiHost::endKeyFromEngine(const char* why) { impl_->endKeyFromEngine(why); }
FoxCommandResult ApiHost::webRemotePtt(bool on) {
    FoxCommandResult r{};
    r.structSize = sizeof(r);
    r.op = FOXAPI_OP_TX_PTT;
    const cascade::core::TxKeyAnswer a = impl_->webRemotePtt(on);
    r.status = a.status;
    r.applied[0] = a.applied0;
    if (a.status != FOXAPI_OK) { r.flags |= FOXAPI_RESULT_REFUSED; }
    copyText(r.message, a.message.c_str());
    return r;
}
void ApiHost::webRemoteBeat() { impl_->webRemoteBeat(); }
void ApiHost::takeDecoderLines(const std::deque<cascade::core::DecodedLine>& log, std::uint64_t total) {
    impl_->takeDecoderLines(log, total);
}
void ApiHost::webRemoteStopped(const char* why) { impl_->webRemoteStopped(why); }
void ApiHost::engineStopping() { impl_->engineStopping(); }
ApiHost::TxFacts ApiHost::txFacts() const { return impl_->txFacts(); }
void ApiHost::setLists(std::shared_ptr<const ApiListBlock> lists) { impl_->setLists(std::move(lists)); }
void ApiHost::endPass() { impl_->endPass(); }

// ---------------------------------------------------------------------------
// The C table
// ---------------------------------------------------------------------------

namespace {

ApiSession* S(FoxSession* s) { return static_cast<ApiSession*>(s); }
ApiHostImpl* H(FoxEngine* e) { return static_cast<ApiHostImpl*>(e); }

// NOTHING THROWS ACROSS THE C BOUNDARY.
template <class F>
int32_t guarded(F&& f) noexcept {
    try {
        return f();
    } catch (...) {
        return FOXAPI_FAILED;
    }
}

template <class F>
int32_t onSession(FoxSession* s, F&& f) noexcept {
    if (s == nullptr) { return FOXAPI_BAD_ARGUMENT; }
    if (S(s)->detached.load(std::memory_order_relaxed)) { return FOXAPI_DETACHED; }
    return guarded([&] { return f(S(s)); });
}

int32_t FOXAPI_CALL apiCreate(const FoxEngineParams* p, FoxEngine** out) {
    if (out == nullptr) { return FOXAPI_BAD_ARGUMENT; }
    *out = nullptr;
    const char* opts = nullptr;
    if (p != nullptr) {
        if (p->structSize < FOXAPI_MIN_ENGINE_PARAMS) { return FOXAPI_BAD_ARGUMENT; }
        opts = p->options;
    }
    return guarded([&] {
        const std::string options = opts != nullptr ? opts : "";
        auto engine = std::make_unique<Engine>();
        engine->configureApi(options);
        engine->initialise();
        ApiHostImpl& host = engine->api().impl();
        if (host.options().running) { (void)engine->applyCommand(cascade::core::cmd::makeInt(FOXAPI_OP_RUN, 1)); }
        // One pass before the control thread starts, so the first snapshot
        // (and the lists) are published before create() returns: a session
        // opened at once reads a real state, never the empty initial one.
        engine->controlPass();
        engine->startControlThread();
        host.owned = engine.release();
        *out = &host;
        return FOXAPI_OK;
    });
}

void FOXAPI_CALL apiDestroy(FoxEngine* e) {
    try {
        if (e == nullptr) { return; }
        Engine* owned = H(e)->owned;
        if (owned == nullptr) { return; }  // the host process's own engine: not the table's to destroy
        owned->shutdown();
        delete owned;
    } catch (...) {
    }
}

int32_t FOXAPI_CALL apiOpenSession(FoxEngine* e, const FoxSessionParams* p, FoxSession** out) {
    if (e == nullptr) { return FOXAPI_BAD_ARGUMENT; }
    return guarded([&] { return H(e)->openSession(p, out); });
}

void FOXAPI_CALL apiCloseSession(FoxSession* s) {
    if (s == nullptr) { return; }
    try {
        S(s)->host->closeSession(S(s));
    } catch (...) {
    }
}

int32_t FOXAPI_CALL apiHeartbeat(FoxSession* s) {
    return onSession(s, [&](ApiSession* x) { return x->host->heartbeat(x); });
}

int32_t FOXAPI_CALL apiReadState(FoxSession* s, FoxReceiverState* out) {
    return onSession(s, [&](ApiSession* x) { return x->host->readState(x, out); });
}

int32_t FOXAPI_CALL apiReadSpectrum(FoxSession* s, uint64_t since, FoxSpectrumInfo* info, float* bins,
                                    uint32_t cap) {
    return onSession(s, [&](ApiSession* x) { return x->host->readSpectrum(since, info, bins, cap); });
}

int32_t FOXAPI_CALL apiSubmit(FoxSession* s, const FoxCommand* c, uint32_t n, FoxSubmitResult* r) {
    return onSession(s, [&](ApiSession* x) { return x->host->submit(x, c, n, r); });
}

int32_t FOXAPI_CALL apiPollResults(FoxSession* s, FoxCommandResult* out, uint32_t cap) {
    return onSession(s, [&](ApiSession* x) { return x->host->pollResults(x, out, cap); });
}

int32_t FOXAPI_CALL apiSubscribe(FoxSession* s, uint64_t mask) {
    return onSession(s, [&](ApiSession* x) { return x->host->subscribe(x, mask); });
}

int32_t FOXAPI_CALL apiPollEvents(FoxSession* s, FoxEvent* out, uint32_t cap) {
    return onSession(s, [&](ApiSession* x) { return x->host->pollEvents(x, out, cap); });
}

int32_t FOXAPI_CALL apiReadAudio(FoxSession* s, uint64_t*, float*, uint32_t, FoxStreamInfo*) {
    return onSession(s, [&](ApiSession*) { return FOXAPI_UNSUPPORTED; });
}

int32_t FOXAPI_CALL apiReadIq(FoxSession* s, uint64_t*, float*, uint32_t, FoxStreamInfo*) {
    return onSession(s, [&](ApiSession*) { return FOXAPI_UNSUPPORTED; });
}

int32_t FOXAPI_CALL apiReadList(FoxSession* s, uint32_t list, uint32_t index, FoxListItem* out) {
    return onSession(s, [&](ApiSession* x) { return x->host->readList(x, list, index, out); });
}

int32_t FOXAPI_CALL apiGetSetting(FoxSession* s, const char* key, char* buf, size_t cap) {
    return onSession(s, [&](ApiSession* x) { return x->host->getSetting(x, key, buf, cap); });
}

int32_t FOXAPI_CALL apiReadImage(FoxSession* s, uint64_t, FoxImageInfo*, uint8_t*, size_t) {
    return onSession(s, [&](ApiSession*) { return FOXAPI_UNSUPPORTED; });
}

int32_t FOXAPI_CALL apiLogin(FoxEngine* e, const char* user, const char* password, char* token, size_t cap) {
    if (e == nullptr) { return FOXAPI_BAD_ARGUMENT; }
    return guarded([&] { return H(e)->login(user, password, token, cap); });
}

int32_t FOXAPI_CALL apiLogout(FoxEngine* e, const char* token) {
    if (e == nullptr) { return FOXAPI_BAD_ARGUMENT; }
    return guarded([&] { return H(e)->logout(token); });
}

int32_t FOXAPI_CALL apiReadScope(FoxSession* s, uint32_t tap, FoxScopeInfo* info, float* out, uint32_t cap) {
    return onSession(s, [&](ApiSession* x) { return x->host->readScope(x, tap, info, out, cap); });
}

int32_t FOXAPI_CALL apiReadNodeSpectrum(FoxSession* s, uint64_t node, uint64_t since, FoxSpectrumInfo* info,
                                        float* bins, uint32_t cap) {
    return onSession(s, [&](ApiSession* x) { return x->host->readNodeSpectrum(node, since, info, bins, cap); });
}

int32_t FOXAPI_CALL apiSubmitText(FoxSession* s, const FoxCommand* c, const char* text, uint32_t len,
                                  FoxSubmitResult* r) {
    return onSession(s, [&](ApiSession* x) { return x->host->submitText(x, c, text, len, r); });
}

const FoxEngineApi kApi = {
    sizeof(FoxEngineApi),
    FOXAPI_VERSION_MAJOR,
    FOXAPI_VERSION_MINOR,
    0,
    // ONLY WHAT IS IMPLEMENTED (engine-stage4.md plan step 1): the receiver,
    // the spectrum, the source and gain commands, the audio DSP, bookmarks,
    // events and the transmitter with its key (API 0.2's rules). Settings,
    // the audio stream and the rest arrive with their steps and set their
    // bits then.
    FOXAPI_CAP_RECEIVER | FOXAPI_CAP_SPECTRUM | FOXAPI_CAP_AUDIO_LEVELS | FOXAPI_CAP_AUDIO_DSP |
        FOXAPI_CAP_EVENTS | FOXAPI_CAP_TRANSMIT |
        // API 0.3 (docs/api-0.3-additions.md).
        FOXAPI_CAP_FACTS | FOXAPI_CAP_LONG_TEXT | FOXAPI_CAP_SCOPE | FOXAPI_CAP_NODE_SPECTRUM,
    "FoxSDR engine",
    CASCADE_VERSION_STRING,
    apiCreate,
    apiDestroy,
    apiOpenSession,
    apiCloseSession,
    apiHeartbeat,
    apiReadState,
    apiReadSpectrum,
    apiSubmit,
    apiPollResults,
    apiSubscribe,
    apiPollEvents,
    apiReadAudio,
    apiReadIq,
    apiReadList,
    apiGetSetting,
    apiReadImage,
    apiLogin,
    apiLogout,
    apiReadScope,
    apiReadNodeSpectrum,
    apiSubmitText,
};

}  // namespace

const FoxEngineApi* engineApi(std::uint32_t apiMajor, std::uint32_t apiMinor) {
    if (apiMajor != FOXAPI_VERSION_MAJOR) { return nullptr; }
    if (FOXAPI_VERSION_MAJOR == 0u && apiMinor != FOXAPI_VERSION_MINOR) { return nullptr; }
    return &kApi;
}

}  // namespace cascade::engine

// The engine module's one entry point (FOXAPI_ENGINE_QUERY_NAME): what a host
// looks up in an engine module, and what the headless engine exports.
extern "C" FOXAPI_EXPORT const FoxEngineApi* FOXAPI_CALL foxsdr_engine_query(uint32_t apiMajor, uint32_t apiMinor) {
    return cascade::engine::engineApi(apiMajor, apiMinor);
}
