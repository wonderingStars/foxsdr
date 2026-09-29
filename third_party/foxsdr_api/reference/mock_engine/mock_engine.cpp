// mock_engine.cpp - see mock_engine.hpp for what this is and is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mock_engine/mock_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/audio_ring.hpp"
#include "common/seqlock.hpp"
#include "common/struct_io.hpp"

// The C handles are opaque to callers; inside the engine they are these.
struct FoxEngine {};
struct FoxSession {};

namespace foxsdr::mock {
namespace {

using SteadyClock = std::chrono::steady_clock;

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}

int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// Fixed facts of the mock receiver
// ---------------------------------------------------------------------------

constexpr double kMaxCentreHz = 6.0e9;
constexpr double kMaxVfoOffsetHz = 1.0e8;
constexpr double kMinBandwidthHz = 100.0;
constexpr double kMaxBandwidthHz = 1.0e7;
constexpr double kMinSquelchDb = -200.0;
constexpr double kMaxSquelchDb = 20.0;
constexpr double kMinDisplayDb = -200.0;
constexpr double kMaxDisplayDb = 40.0;
constexpr double kMinDbSpan = 10.0;
constexpr double kAudioRateHz = 48000.0;
constexpr std::size_t kMaxPendingPerSession = FOXAPI_MAX_PENDING;  // counted until POLLED
// The commands that can only make the transmitter safer, one kind each:
// TX_PTT 0, TX_LATCH 0, RUN 0, TX_CLOSE, TX_REMOTE_ARM 0 (saferKind()).
constexpr int kSaferKinds = 5;
static_assert(kSaferKinds == static_cast<int>(FOXAPI_SAFETY_RESERVE),
              "one outstanding safer command per kind is the whole reserve");
// Places of the session limit that only a LOCAL session may take, so remote
// logins can never lock the local window out (round-4 review).
constexpr int kLocalReservedPlaces = 4;
constexpr std::size_t kMaxEventsPerSession = 256;
constexpr std::size_t kMaxBookmarks = 1000;
constexpr int kLoginFailuresBeforeLockout = 5;

struct ModeInfo {
    uint32_t mode;
    const char* name;
    double defaultBandwidthHz;
};
constexpr ModeInfo kModes[] = {
    {FOXAPI_DEMOD_NFM, "NFM", 12500.0}, {FOXAPI_DEMOD_WFM, "WFM", 150000.0},
    {FOXAPI_DEMOD_AM, "AM", 10000.0},   {FOXAPI_DEMOD_DSB, "DSB", 6000.0},
    {FOXAPI_DEMOD_USB, "USB", 3000.0},  {FOXAPI_DEMOD_CW, "CW", 3000.0},
    {FOXAPI_DEMOD_LSB, "LSB", 3000.0},  {FOXAPI_DEMOD_RAW, "RAW", 200000.0},
};
constexpr double kBandwidths[] = {200000.0, 150000.0, 12500.0, 10000.0, 6000.0, 3000.0};

const ModeInfo* findMode(uint32_t mode) {
    for (const auto& m : kModes) {
        if (m.mode == mode) {
            return &m;
        }
    }
    return nullptr;
}

struct Device {
    const char* id;
    const char* name;
    std::vector<double> rates;
    bool hasGain;
};

const std::vector<Device>& devices() {
    static const std::vector<Device> d = {
        {"siggen", "Signal generator", {2048000.0}, false},
        {"mock:rtl:00000001", "Mock RTL-SDR (serial 00000001)",
         {250000.0, 1024000.0, 1400000.0, 2048000.0, 2400000.0, 3200000.0}, true},
    };
    return d;
}

struct TxDevice {
    const char* id;
    const char* name;
};
constexpr TxDevice kTxDevices[] = {{"mock:tx:dummy", "Mock transmitter (dummy load, no RF)"}};

// A synthetic station: a flat-topped hump of `widthHz` peaking at `peakDb`.
struct Station {
    double hz;
    double widthHz;
    double peakDb;
};

std::vector<Station> makeStations() {
    std::vector<Station> s;
    // Broadcast FM: deterministic, irregular spacing, varied strength.
    uint32_t lcg = 12345u;
    auto next = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return (lcg >> 8) / static_cast<double>(1u << 24);
    };
    for (double f = 87.7e6; f < 108.0e6;) {
        s.push_back({f, 180000.0, -68.0 + 34.0 * next()});
        f += 200000.0 * (1 + static_cast<int>(next() * 4.0));
    }
    s.push_back({94.62e6, 180000.0, -32.0});   // the strong local station
    s.push_back({145.5e6, 12500.0, -55.0});    // 2 m FM calling
    s.push_back({146.0e6, 12500.0, -71.0});
    s.push_back({446.00625e6, 12500.0, -60.0}); // PMR446 channel 1
    s.push_back({118.7e6, 8000.0, -62.0});     // airband AM
    s.push_back({124.325e6, 8000.0, -58.0});
    s.push_back({1090.0e6, 2000000.0, -63.0}); // ADS-B, smeared
    s.push_back({7.074e6, 2500.0, -64.0});     // FT8 on 40 m
    return s;
}

double dbToLin(double db) { return std::pow(10.0, db / 10.0); }
double linToDb(double lin) { return 10.0 * std::log10(std::max(lin, 1e-30)); }

// Super-Gaussian: flat top, steep skirts. 1 at the centre.
double humpShape(double offsetHz, double widthHz) {
    const double x = offsetHz / (0.5 * widthHz);
    const double x2 = x * x;
    return std::exp(-(x2 * x2 * x2));
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    int bins = 2048;
    double fps = 30.0;
    double centreHz = 94.5e6;
    bool running = true;
    int pttHoldMs = FOXAPI_PTT_HOLD_MS;
    int latchTimeoutMs = FOXAPI_LATCH_TIMEOUT_MS;
    int keepaliveMs = FOXAPI_KEEPALIVE_MS;
    int remoteKeepaliveMs = FOXAPI_REMOTE_KEEPALIVE_MS;
    int controlHz = 200;
    std::string token;  // a configured token; empty = only login() tokens
    std::string user;   // login() credential; both empty = login refused
    std::string password;
    int maxSessions = 64;        // all sessions
    int maxRemoteSessions = 16;  // of which REMOTE (docs/TRANSPORTS.md 2.5)
    int loginLockoutMs = 60000;
    int64_t tokenTtlMs = int64_t{12} * 3600 * 1000;
};

Options parseOptions(const char* text) {
    Options o;
    if (text == nullptr) {
        return o;
    }
    std::string all(text);
    std::size_t pos = 0;
    while (pos <= all.size()) {
        std::size_t end = all.find(';', pos);
        if (end == std::string::npos) {
            end = all.size();
        }
        const std::string kv = all.substr(pos, end - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq);
            const std::string v = kv.substr(eq + 1);
            const double d = std::atof(v.c_str());
            if (k == "bins") {
                o.bins = std::clamp(static_cast<int>(d), 64, 16384);
            } else if (k == "fps") {
                o.fps = std::clamp(d, 1.0, 240.0);
            } else if (k == "centreHz" && std::isfinite(d)) {
                o.centreHz = std::clamp(d, 1.0e5, kMaxCentreHz);
            } else if (k == "running") {
                o.running = d != 0.0;
            } else if (k == "pttHoldMs") {
                // A test may SHORTEN a safety timer. Nothing may lengthen one.
                o.pttHoldMs = std::clamp(static_cast<int>(d), 10, FOXAPI_PTT_HOLD_MS);
            } else if (k == "latchTimeoutMs") {
                o.latchTimeoutMs = std::clamp(static_cast<int>(d), 10, FOXAPI_LATCH_TIMEOUT_MS);
            } else if (k == "keepaliveMs") {
                o.keepaliveMs = std::clamp(static_cast<int>(d), 10, FOXAPI_KEEPALIVE_MS);
            } else if (k == "remoteKeepaliveMs") {
                o.remoteKeepaliveMs = std::clamp(static_cast<int>(d), 10, FOXAPI_REMOTE_KEEPALIVE_MS);
            } else if (k == "controlHz") {
                o.controlHz = std::clamp(static_cast<int>(d), 20, 1000);
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
                // Test option: may only SHORTEN the lockout.
                o.loginLockoutMs = std::clamp(static_cast<int>(d), 10, 60000);
            } else if (k == "tokenTtlMs") {
                o.tokenTtlMs = std::clamp(static_cast<int64_t>(d), int64_t{10}, int64_t{12} * 3600 * 1000);
            }
        }
        pos = end + 1;
    }
    return o;
}

// ---------------------------------------------------------------------------
// The spectrum stream: a ring of slots, so a reader copying the newest frame
// is never overwritten mid-copy unless it is slower than kSlots frames - and
// then it notices and retries. The writer never waits for anyone.
// ---------------------------------------------------------------------------

class SpectrumRing {
public:
    static constexpr int kSlots = 4;

    explicit SpectrumRing(int maxBins) : maxBins_(maxBins) {
        for (auto& s : slots_) {
            s.data = std::make_unique<std::atomic<float>[]>(static_cast<std::size_t>(maxBins));
        }
    }

    void publish(const std::vector<float>& bins, double centreHz, double spanHz, int64_t ums) {
        const uint64_t seq = ++lastSeq_;
        const int idx = static_cast<int>(seq % kSlots);
        Slot& s = slots_[idx];
        s.seq.store(0, std::memory_order_relaxed);  // "being written"
        std::atomic_thread_fence(std::memory_order_release);
        const auto n = static_cast<uint32_t>(std::min<std::size_t>(bins.size(), maxBins_));
        for (uint32_t i = 0; i < n; ++i) {
            s.data[i].store(bins[i], std::memory_order_relaxed);
        }
        s.bins.store(n, std::memory_order_relaxed);
        s.centreHz.store(centreHz, std::memory_order_relaxed);
        s.spanHz.store(spanHz, std::memory_order_relaxed);
        s.unixMs.store(ums, std::memory_order_relaxed);
        s.seq.store(seq, std::memory_order_release);
        latest_.store(seq, std::memory_order_release);
    }

    // FOXAPI_OK, FOXAPI_NO_CHANGE or FOXAPI_BUSY.
    int32_t read(uint64_t since, FoxSpectrumInfo& info, float* out, uint32_t cap) const {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint64_t seq = latest_.load(std::memory_order_acquire);
            if (seq == 0 || seq <= since) {
                return FOXAPI_NO_CHANGE;
            }
            const Slot& s = slots_[seq % kSlots];
            if (s.seq.load(std::memory_order_acquire) != seq) {
                continue;  // overtaken already: take the newer one
            }
            const uint32_t n = s.bins.load(std::memory_order_relaxed);
            info.binCount = n;
            info.seq = seq;
            info.centreHz = s.centreHz.load(std::memory_order_relaxed);
            info.spanHz = s.spanHz.load(std::memory_order_relaxed);
            info.unixMs = s.unixMs.load(std::memory_order_relaxed);
            info.flags = 0;
            const uint32_t outN = (out == nullptr) ? 0u : std::min(cap, n);
            if (outN == n) {
                for (uint32_t i = 0; i < n; ++i) {
                    out[i] = s.data[i].load(std::memory_order_relaxed);
                }
            } else if (outN > 0) {
                // Fewer bins than the engine has: the MAXIMUM of each group,
                // so a narrow carrier survives the reduction.
                for (uint32_t i = 0; i < outN; ++i) {
                    const uint32_t a = static_cast<uint32_t>(uint64_t(i) * n / outN);
                    uint32_t b = static_cast<uint32_t>(uint64_t(i + 1) * n / outN);
                    b = std::max(b, a + 1);
                    float m = -1e30f;
                    for (uint32_t k = a; k < b; ++k) {
                        m = std::max(m, s.data[k].load(std::memory_order_relaxed));
                    }
                    out[i] = m;
                }
            }
            info.copied = outN;
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.seq.load(std::memory_order_relaxed) == seq) {
                return FOXAPI_OK;
            }
        }
        return FOXAPI_BUSY;
    }

    uint64_t latestSeq() const { return latest_.load(std::memory_order_acquire); }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint32_t> bins{0};
        std::atomic<double> centreHz{0.0};
        std::atomic<double> spanHz{0.0};
        std::atomic<int64_t> unixMs{0};
        std::unique_ptr<std::atomic<float>[]> data;
    };
    std::size_t maxBins_;
    std::array<Slot, kSlots> slots_;
    std::atomic<uint64_t> latest_{0};
    uint64_t lastSeq_ = 0;  // writer only
};

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

struct Session : FoxSession {
    uint64_t id = 0;
    uint32_t flags = 0;
    uint64_t grants = 0;
    std::string clientName;
    class Engine* engine = nullptr;
    std::atomic<int64_t> lastBeatMs{0};
    std::string token;                 // the login token it was opened with ("" = none)
    // WHO this session acts for: "local" for every LOCAL session (in-process
    // or through the local token file - the operator at this machine), and
    // "token:<token>" for a remote one. Latch marks belong to the principal,
    // never to a session, because a session can close and reopen.
    std::string principal;
    std::atomic<bool> detached{false}; // its token was revoked: every call answers DETACHED

    std::mutex m;  // guards everything below, down to `safer`
    std::deque<FoxCommandResult> results;
    std::deque<FoxEvent> events;
    uint64_t eventMask = 0;
    uint64_t eventSeq = 0;
    uint64_t eventsDropped = 0;
    std::size_t pending = 0;  // ordinary commands outstanding (safer ones are in `safer`)
    uint64_t lastTicket = 0;  // tickets are per session, from 1, increasing
    // One outstanding ticket per safer kind. A safer command of a kind that
    // already has one (queued, or answered but not yet read) is MERGED with
    // it: it is answered FOXAPI_NO_CHANGE with that ticket, is applied in its
    // turn, and the ticket's ONE result reports the latest application.
    struct SaferSlot {
        uint64_t ticket = 0;     // 0 = none outstanding
        int copies = 0;          // queued applications not yet made
        bool inResults = false;  // its result is waiting in `results`
        bool read = false;       // its result was read while copies remained
    };
    std::array<SaferSlot, kSaferKinds> safer{};

    // FOXAPI_RX_TX_LATCH_RELEASE_FIRST for this session: written by the
    // control thread at the end of every pass, read by read_state.
    std::atomic<bool> releaseFirst{false};
};

// What the control thread publishes: the shared state plus who holds the key.
struct Published {
    FoxReceiverState st;
    uint64_t keyOwner;
    bool localMarked;  // the "local" principal is marked: a new local session must release first
};

struct Pending {
    FoxCommand cmd;
    uint64_t ticket;
    uint64_t sessionId;
    uint32_t sessionFlags;
    int32_t refused;         // FOXAPI_OK = apply it; else the refusal to deliver, in order
    std::string principal;   // Session::principal of the sender
    int saferKind = -1;      // saferKind(cmd), -1 for every other command
    bool merged = false;     // a safer command merged into its kind's outstanding ticket (NO_CHANGE)
};

// The app's bookmark (core/freq_manager.hpp): an imported frequency list is
// just bookmarks with a group, so there is no separate "frequency list".
struct Bookmark {
    uint64_t id;
    std::string name;
    double hz;
    uint32_t mode;
    double bandwidthHz;
    std::string group;  // "" = ungrouped
    bool favourite;
};

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

class Engine : public FoxEngine {
public:
    explicit Engine(const Options& o)
        : opt_(o), spectrum_(o.bins), stations_(makeStations()) {
        m_.centreHz = o.centreHz;
        m_.running = o.running;
        publishParams();
        publishState();
        run_.store(true);
        signalThread_ = std::thread([this] { signalLoop(); });
        controlThread_ = std::thread([this] { controlLoop(); });
    }

    ~Engine() {
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            run_.store(false);
        }
        queueCv_.notify_all();
        if (controlThread_.joinable()) {
            controlThread_.join();
        }
        if (signalThread_.joinable()) {
            signalThread_.join();
        }
    }

    // ---- sessions ----------------------------------------------------------

    int32_t openSession(const FoxSessionParams* p, FoxSession** out) {
        if (p == nullptr || out == nullptr || p->structSize < FOXAPI_MIN_SESSION_PARAMS) {
            return FOXAPI_BAD_ARGUMENT;
        }
        const bool remote = (p->flags & FOXAPI_SESSION_REMOTE) != 0u;
        const bool local = (p->flags & FOXAPI_SESSION_LOCAL) != 0u;
        if (remote == local) {
            return FOXAPI_BAD_ARGUMENT;  // exactly one of the two
        }
        uint64_t grants = p->grants | FOXAPI_GRANT_VIEW;
        std::string token;
        if (remote) {
            if (p->token == nullptr || !tokenValid(p->token)) {
                return FOXAPI_UNAUTHENTICATED;
            }
            token = p->token;
            grants &= ~FOXAPI_GRANT_ADMIN;  // administration is a local act
        }
        // Test seam (mock.openDelay): hold this open between the token check
        // and the insert, so a test can revoke the token inside that gap.
        if (const int delay = openDelayMs_.load(std::memory_order_relaxed); delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        auto s = std::make_shared<Session>();
        s->flags = p->flags;
        s->grants = grants;
        s->clientName = p->clientName ? p->clientName : "";
        s->engine = this;
        s->token = token;
        s->principal = remote ? "token:" + token : std::string("local");
        if (!remote) {
            Published snap{};
            s->releaseFirst.store(state_.load(snap) && snap.localMarked);
        }
        s->lastBeatMs.store(steadyMs());
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            const int open = static_cast<int>(sessions_.size());  // detached ones are closed by the engine
            if (open >= opt_.maxSessions) {
                return FOXAPI_LIMIT;  // a bounded number of sessions, as of everything
            }
            if (remote) {
                // The last kLocalReservedPlaces places are only ever given to
                // LOCAL sessions: however many remote logins come and go, the
                // local window can always open (round-4 review).
                if (open >= opt_.maxSessions - kLocalReservedPlaces) {
                    return FOXAPI_LIMIT;
                }
                // ATTACHED remote sessions: a detached one holds nothing and
                // can do nothing, so it does not keep a new remote out while
                // the engine gets round to closing it (round-3 review).
                int remotes = 0;
                for (const auto& kv : sessions_) {
                    remotes += ((kv.second->flags & FOXAPI_SESSION_REMOTE) != 0u &&
                                !kv.second->detached.load())
                                   ? 1 : 0;
                }
                if (remotes >= opt_.maxRemoteSessions) {
                    return FOXAPI_LIMIT;
                }
            }
            s->id = ++nextSessionId_;
            sessions_[s->id] = s;
        }
        // The token may have expired, or been revoked, between the check
        // above and the insert, with its detach pass already run over a
        // session list that did not yet hold this one. Check again now that
        // it is listed: either the token is still good (and any later drop
        // will find this session) or this session goes.
        if (remote && !tokenValid(token.c_str())) {
            {
                std::lock_guard<std::mutex> lk(sessionsMutex_);
                sessions_.erase(s->id);
            }
            return FOXAPI_UNAUTHENTICATED;
        }
        *out = s.get();
        return FOXAPI_OK;
    }

    void closeSession(Session* s) {
        std::shared_ptr<Session> keep;
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            auto it = sessions_.find(s->id);
            if (it == sessions_.end()) {
                // Detached and already closed by the engine (reapDetached):
                // only its handle was left, and this frees it.
                reaped_.erase(s->id);
                return;
            }
            keep = it->second;
            sessions_.erase(it);
        }
        {
            // The control thread releases the key on its next pass (at once:
            // it is woken here) because only it may change the key.
            std::lock_guard<std::mutex> lk(queueMutex_);
            closedSessions_.push_back(keep->id);
        }
        queueCv_.notify_all();
        // `keep` goes out of scope here; the control thread holds its own
        // shared_ptr for as long as it is delivering to this session.
    }

    // ---- login ---------------------------------------------------------------

    int32_t login(const char* user, const char* password, char* out, std::size_t cap) {
        if (user == nullptr || password == nullptr || out == nullptr || cap < 65) {
            return FOXAPI_BAD_ARGUMENT;
        }
        const int64_t now = steadyMs();
        std::lock_guard<std::mutex> lk(tokensMutex_);
        if (lockedUntilMs_ > now) {
            return FOXAPI_LIMIT;  // throttled: the answer is the same right or wrong
        }
        if (opt_.user.empty() || opt_.password.empty() || opt_.user != user ||
            opt_.password != password) {
            if (++loginFailures_ >= kLoginFailuresBeforeLockout) {
                loginFailures_ = 0;
                lockedUntilMs_ = now + opt_.loginLockoutMs;
            }
            return FOXAPI_UNAUTHENTICATED;
        }
        loginFailures_ = 0;
        // 256 random bits, as hex. (The real engine keeps only a hash of
        // each token; the mock keeps the token, which is fine for a mock.)
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

    // OK when a live token was revoked; NOT_FOUND for a token that is unknown,
    // already revoked or EXPIRED - whose sessions are detached all the same;
    // DENIED for the engine's CONFIGURED token, which is configuration, not a
    // login: logout cannot revoke it and detaches nothing (round-4 review: it
    // answered NOT_FOUND, detached every session on it, and the token went on
    // opening new ones).
    int32_t logout(const char* token) {
        if (token == nullptr) {
            return FOXAPI_BAD_ARGUMENT;
        }
        if (!opt_.token.empty() && opt_.token == token) {
            return FOXAPI_DENIED;
        }
        bool live = false;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            auto it = tokens_.find(token);
            live = it != tokens_.end() && steadyMs() < it->second;
        }
        const bool erased = dropToken(token);
        return (live && erased) ? FOXAPI_OK : FOXAPI_NOT_FOUND;
    }

    // Every session opened with `token` is detached now, and the control
    // thread releases any key or consent it held on its next pass.
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
        if (ids.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            closedSessions_.insert(closedSessions_.end(), ids.begin(), ids.end());
        }
        queueCv_.notify_all();
    }

    // A session does not outlive its token: expired tokens are dropped and
    // their sessions detached (control thread, once a pass).
    void expireTokens() {
        std::vector<std::string> gone;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            const int64_t now = steadyMs();
            for (const auto& kv : tokens_) {
                if (now >= kv.second) {
                    gone.push_back(kv.first);
                }
            }
        }
        for (const std::string& t : gone) {
            dropToken(t);
        }
    }

    // Whether `t` may open or keep a session now. An expired login token is
    // DROPPED here, and dropping always detaches: a token is only ever erased
    // together with its sessions (dropToken), never on its own. (Round-3
    // review: this used to erase an expired token by itself, so the control
    // thread's expiry pass never saw it and its sessions outlived it.)
    bool tokenValid(const char* t) {
        if (!opt_.token.empty() && opt_.token == t) {
            return true;
        }
        bool expired = false;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            auto it = tokens_.find(t);
            if (it == tokens_.end()) {
                return false;
            }
            if (steadyMs() < it->second) {
                return true;
            }
            expired = true;
        }
        if (expired) {
            dropToken(t);
        }
        return false;
    }

    // Erases a login token AND detaches every session opened with it - the
    // one way a token ever goes. True when this call erased it.
    bool dropToken(const std::string& token) {
        bool erased = false;
        {
            std::lock_guard<std::mutex> lk(tokensMutex_);
            erased = tokens_.erase(token) != 0;
        }
        // Detached even when another thread erased it first: detaching is
        // idempotent, and a session must never be left attached to a token
        // that is gone.
        detachToken(token);
        return erased;
    }

    int32_t heartbeat(Session* s) {
        s->lastBeatMs.store(steadyMs(), std::memory_order_relaxed);
        return FOXAPI_OK;
    }

    // ---- reads -------------------------------------------------------------

    int32_t readState(Session* s, FoxReceiverState* out) {
        if (out == nullptr || out->structSize < FOXAPI_MIN_RECEIVER_STATE) {
            return FOXAPI_BAD_ARGUMENT;
        }
        Published p;
        if (!state_.load(p)) {
            return FOXAPI_BUSY;
        }
        p.st.grants = s->grants;
        if (p.keyOwner != 0 && p.keyOwner == s->id) {
            p.st.flags |= FOXAPI_RX_TX_KEY_MINE;
        }
        if (s->releaseFirst.load(std::memory_order_relaxed)) {
            p.st.flags |= FOXAPI_RX_TX_LATCH_RELEASE_FIRST;
        }
        return copyOut(p.st, out, FOXAPI_MIN_RECEIVER_STATE);
    }

    int32_t readSpectrum(uint64_t since, FoxSpectrumInfo* info, float* bins, uint32_t cap) {
        if (info == nullptr || info->structSize < FOXAPI_MIN_SPECTRUM_INFO ||
            (bins == nullptr && cap != 0)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        FoxSpectrumInfo local{};
        local.structSize = sizeof(local);
        const int32_t r = spectrum_.read(since, local, bins, cap);
        if (r != FOXAPI_OK) {
            return r;
        }
        copyOut(local, info, FOXAPI_MIN_SPECTRUM_INFO);
        return FOXAPI_OK;
    }

    int32_t readAudio(Session* s, uint64_t* cursor, float* out, uint32_t cap, FoxStreamInfo* info) {
        if ((s->grants & FOXAPI_GRANT_AUDIO) == 0u) {
            return FOXAPI_DENIED;
        }
        if (cursor == nullptr || (out == nullptr && cap != 0)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        uint64_t dropped = 0;
        // A short info is refused BEFORE the cursor moves, never skipped:
        // a caller that silently got no drop count would not know it lost audio.
        if (info != nullptr && info->structSize < FOXAPI_MIN_STREAM_INFO) {
            return FOXAPI_BAD_ARGUMENT;
        }
        const std::size_t n = audio_.read(*cursor, out, cap, dropped);
        if (info != nullptr) {
            FoxStreamInfo local{};
            local.structSize = sizeof(local);
            local.channels = 1;
            local.rateHz = kAudioRateHz;
            local.dropped = dropped;
            local.written = audio_.written();
            copyOut(local, info, FOXAPI_MIN_STREAM_INFO);
        }
        return static_cast<int32_t>(n);
    }

    int32_t readList(uint32_t list, uint32_t index, FoxListItem* out) {
        if (out != nullptr && out->structSize < FOXAPI_MIN_LIST_ITEM) {
            return FOXAPI_BAD_ARGUMENT;
        }
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.list = list;
        it.index = index;
        int32_t count = 0;
        switch (list) {
        case FOXAPI_LIST_MODES: {
            count = static_cast<int32_t>(std::size(kModes));
            if (index < std::size(kModes)) {
                const auto& m = kModes[index];
                it.id = m.mode;
                it.value[0] = m.mode;
                it.value[1] = m.defaultBandwidthHz;
                copyText(it.name, m.name);
            }
            break;
        }
        case FOXAPI_LIST_BANDWIDTHS: {
            count = static_cast<int32_t>(std::size(kBandwidths));
            if (index < std::size(kBandwidths)) {
                it.value[0] = kBandwidths[index];
                char label[32];
                std::snprintf(label, sizeof(label), "%gk", kBandwidths[index] / 1000.0);
                copyText(it.name, label);
            }
            break;
        }
        case FOXAPI_LIST_DEVICES: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            count = scanned_ ? static_cast<int32_t>(devices().size()) : 1;
            if (static_cast<int32_t>(index) < count) {
                const Device& d = devices()[index];
                copyText(it.name, d.name);
                copyText(it.detail, d.id);
                it.id = index + 1u;
                if (static_cast<int>(index) == deviceIndexListed_) {
                    it.flags |= FOXAPI_ITEM_ACTIVE;
                }
            }
            break;
        }
        case FOXAPI_LIST_SAMPLE_RATES: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            const Device& d = devices()[static_cast<std::size_t>(deviceIndexListed_)];
            count = static_cast<int32_t>(d.rates.size());
            if (index < d.rates.size()) {
                it.value[0] = d.rates[index];
            }
            break;
        }
        case FOXAPI_LIST_GAINS: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            const Device& d = devices()[static_cast<std::size_t>(deviceIndexListed_)];
            count = d.hasGain ? 1 : 0;
            if (static_cast<int32_t>(index) < count) {
                copyText(it.name, "TUNER");
                it.value[0] = 0.0;
                it.value[1] = 49.6;
                it.value[2] = 0.1;
                it.value[3] = gainListed_;
            }
            break;
        }
        case FOXAPI_LIST_BOOKMARKS: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            count = static_cast<int32_t>(bookmarks_.size());
            if (index < bookmarks_.size()) {
                const Bookmark& b = bookmarks_[index];
                it.id = b.id;
                copyText(it.name, b.name.c_str());
                copyText(it.detail, b.group.c_str());
                it.value[0] = b.hz;
                it.value[1] = b.mode;
                it.value[2] = b.bandwidthHz;
                if (b.favourite) {
                    it.flags |= FOXAPI_ITEM_FAVOURITE;
                }
            }
            break;
        }
        case FOXAPI_LIST_BOOKMARK_GROUPS: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            std::map<std::string, int> groups;  // named groups, sorted by name
            for (const Bookmark& b : bookmarks_) {
                if (!b.group.empty()) {
                    ++groups[b.group];
                }
            }
            count = static_cast<int32_t>(groups.size());
            if (index < groups.size()) {
                auto g = std::next(groups.begin(), static_cast<std::ptrdiff_t>(index));
                copyText(it.name, g->first.c_str());
                it.value[0] = g->second;
            }
            break;
        }
        case FOXAPI_LIST_TX_DEVICES: {
            count = static_cast<int32_t>(std::size(kTxDevices));
            if (index < std::size(kTxDevices)) {
                copyText(it.name, kTxDevices[index].name);
                copyText(it.detail, kTxDevices[index].id);
                it.id = index + 1u;
            }
            break;
        }
        case FOXAPI_LIST_AUDIO_DEVICES: {
            count = 1;
            if (index == 0) {
                copyText(it.name, "Mock speakers (nothing is played)");
                copyText(it.detail, "mock:speakers");
                it.flags |= FOXAPI_ITEM_ACTIVE;
                it.id = 1;
            }
            break;
        }
        default:
            return FOXAPI_UNSUPPORTED;
        }
        if (out != nullptr && static_cast<int32_t>(index) < count) {
            copyOut(it, out, FOXAPI_MIN_LIST_ITEM);
        }
        return count;
    }

    int32_t getSetting(const char* key, char* buf, std::size_t cap) {
        if (key == nullptr || (buf == nullptr && cap != 0)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        std::string v;
        if (std::strcmp(key, "mock.queueDepth") == 0) {
            // Test seam: commands waiting for the control thread right now.
            std::lock_guard<std::mutex> lk(queueMutex_);
            v = std::to_string(queue_.size());
        } else if (std::strcmp(key, "mock.keyState") == 0) {
            // Test seam: the key, the latch, the marks and exemptions as the
            // last control pass left them (keyStateText()).
            std::lock_guard<std::mutex> lk(settingsMutex_);
            v = keyState_;
        } else {
            std::lock_guard<std::mutex> lk(settingsMutex_);
            auto it = settings_.find(key);
            if (it == settings_.end()) {
                if (std::strcmp(key, "spectrum.bins") == 0) {
                    v = std::to_string(opt_.bins);
                } else if (std::strcmp(key, "spectrum.fps") == 0) {
                    v = std::to_string(opt_.fps);
                } else {
                    return FOXAPI_NOT_FOUND;
                }
            } else {
                v = it->second;
            }
        }
        if (cap > 0) {
            const std::size_t n = std::min(v.size(), cap - 1);
            std::memcpy(buf, v.data(), n);
            buf[n] = '\0';
        }
        return static_cast<int32_t>(v.size());
    }

    // ---- commands ----------------------------------------------------------

    int32_t submit(Session* s, const FoxCommand* cmds, uint32_t count, FoxSubmitResult* results) {
        if (count == 0) {
            return 0;
        }
        // Whole-call answers first, and the batch bound BEFORE anything is
        // sized by the caller's count.
        if (count > FOXAPI_MAX_BATCH) {
            return FOXAPI_LIMIT;
        }
        if (cmds == nullptr || cmds[0].structSize < FOXAPI_MIN_COMMAND) {
            return FOXAPI_BAD_ARGUMENT;
        }
        const std::size_t stride = cmds[0].structSize;
        std::size_t rstride = 0;
        if (results != nullptr) {
            // The caller's stride, whatever this engine's own sizeof has
            // grown to (rule 4): only a stride below the minimum is refused.
            rstride = results[0].structSize;
            if (rstride < FOXAPI_MIN_SUBMIT_RESULT) {
                return FOXAPI_BAD_ARGUMENT;
            }
        }
        // A session opened with a login token lives only as long as the token.
        if (!s->token.empty() && !tokenValid(s->token.c_str())) {
            detachToken(s->token);
            return FOXAPI_DETACHED;
        }
        Published snap;
        const bool haveSnap = state_.load(snap);
        int32_t taken = 0;
        std::vector<Pending> batch;
        batch.reserve(count);  // bounded by FOXAPI_MAX_BATCH above
        // Held through the enqueue below, so the merge decided here and the
        // queue agree (lock order: session, then queue; nothing takes them
        // the other way round).
        std::lock_guard<std::mutex> lk(s->m);
        for (uint32_t i = 0; i < count; ++i) {
            const FoxCommand c = copyInAt<FoxCommand>(cmds, stride, i);
            FoxSubmitResult r{};
            r.structSize = sizeof(r);
            // Judged now, against the published snapshot - so a key request
            // with no transmitter behind it can never sit in the queue
            // waiting for one - but ANSWERED as a result, in ticket order
            // with everything else this session sent.
            const int32_t refused = validate(*s, c, haveSnap ? &snap : nullptr);
            // A safer command refused at submit (malformed, no grant, a
            // remote's LATCH 0) changes nothing, so it is NOT a safer
            // command: it is never merged, never replaces a valid one of its
            // kind, and takes its own ticket and its own refusal as an
            // ordinary command does (round-5 review: a NaN LATCH 0 sent after
            // a valid one replaced it in the queue, and the latch stayed
            // closed).
            const int kind = refused == FOXAPI_OK ? saferKind(c) : -1;
            if (kind < 0 && s->pending >= kMaxPendingPerSession) {
                r.status = FOXAPI_BUSY;  // not taken: no ticket, no result
            } else {
                Pending p{c, 0, s->id, s->flags, refused, s->principal, kind, false};
                if (kind < 0) {
                    r.status = FOXAPI_OK;
                    r.ticket = ++s->lastTicket;
                    ++s->pending;
                    ++taken;
                } else {
                    // SAFER COMMANDS ARE NEVER BUSY, AND NEVER PILE UP
                    // (round-4 review: 16 PTT 0s used a shared reserve and
                    // then LATCH 0 and RUN 0 were BUSY with the latch held).
                    // One ticket per kind is outstanding at most; a newer one
                    // of the kind is merged with it.
                    Session::SaferSlot& slot = s->safer[static_cast<std::size_t>(kind)];
                    if (slot.ticket != 0 && !slot.read) {
                        r.status = FOXAPI_NO_CHANGE;  // merged: that ticket's one result answers it
                        r.ticket = slot.ticket;
                        p.merged = true;
                    } else {
                        slot = Session::SaferSlot{};
                        slot.ticket = ++s->lastTicket;
                        r.status = FOXAPI_OK;
                        r.ticket = slot.ticket;
                        ++taken;
                    }
                }
                p.ticket = r.ticket;
                batch.push_back(std::move(p));
            }
            if (results != nullptr) {
                copyOutAt(r, results, rstride, i);
            }
        }
        if (!batch.empty()) {
            {
                std::lock_guard<std::mutex> lq(queueMutex_);
                submitted_ += batch.size();  // test seam: mock.gather counts these
                for (auto& p : batch) {
                    enqueueLocked(*s, std::move(p));
                }
            }
            queueCv_.notify_all();
        }
        return taken;
    }

    int32_t pollResults(Session* s, FoxCommandResult* out, uint32_t cap) {
        if (cap == 0) {
            return 0;
        }
        if (out == nullptr || out[0].structSize < FOXAPI_MIN_COMMAND_RESULT) {
            return FOXAPI_BAD_ARGUMENT;
        }
        const std::size_t stride = out[0].structSize;
        std::lock_guard<std::mutex> lk(s->m);
        uint32_t n = 0;
        while (n < cap && !s->results.empty()) {
            const FoxCommandResult& r = s->results.front();
            copyOutAt(r, out, stride, n);
            // A command stops counting as outstanding only now, when its
            // result has been READ - so unread results are bounded by
            // FOXAPI_MAX_PENDING plus one per safer kind, and never dropped.
            bool safer = false;
            for (Session::SaferSlot& slot : s->safer) {
                if (slot.ticket != 0 && slot.ticket == r.ticket && slot.inResults) {
                    slot.inResults = false;
                    if (slot.copies == 0) {
                        slot = Session::SaferSlot{};  // the kind is free again
                    } else {
                        slot.read = true;  // answered; later copies apply silently
                    }
                    safer = true;
                    break;
                }
            }
            if (!safer && s->pending > 0) {
                --s->pending;
            }
            s->results.pop_front();
            ++n;
        }
        return static_cast<int32_t>(n);
    }

    int32_t subscribe(Session* s, uint64_t mask) {
        std::lock_guard<std::mutex> lk(s->m);
        s->eventMask = mask;
        return FOXAPI_OK;
    }

    int32_t pollEvents(Session* s, FoxEvent* out, uint32_t cap) {
        if (cap == 0) {
            return 0;
        }
        if (out == nullptr || out[0].structSize < FOXAPI_MIN_EVENT) {
            return FOXAPI_BAD_ARGUMENT;
        }
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

private:
    // The control-thread-owned model of the receiver. Nothing else touches it.
    struct Model {
        bool running = true;
        bool faulted = false;
        std::string faultMessage;
        double centreHz = 94.5e6;
        double vfoOffsetHz = 120000.0;
        double sampleRateHz = 2048000.0;
        double bandwidthHz = 150000.0;
        double squelchDb = -120.0;
        double volume = 0.5;
        bool muted = false;
        uint32_t mode = FOXAPI_DEMOD_WFM;
        uint32_t deemphasis = 0;
        bool stereo = true;
        bool nr = false;
        double nrStrength = 0.5;
        bool notch = false;
        double notchHz = 1000.0;
        double notchQ = 30.0;
        bool autoNotch = false;
        double dbMin = -110.0;
        double dbMax = 0.0;
        int deviceIndex = 0;
        double gainDb = 29.7;
        bool deviceAgc = false;
        // The transmitter.
        bool txOpen = false;
        uint32_t txMode = FOXAPI_TX_MODE_NFM;
        double txFrequencyHz = 145.5e6;
        double txPowerDb = -89.75;
        uint64_t keyOwner = 0;       // session holding the key, 0 = none
        bool keyOwnerRemote = false; // ...and whether it is a REMOTE session
        std::string keyOwnerPrincipal;  // ...and whom it acts for (Session::principal)
        uint64_t remoteArmedBy = 0;  // LOCAL session whose consent allows remote PTT, 0 = none
        int64_t holdUntilMs = 0;     // PTT hold deadline, 0 = no hold
        bool latched = false;
        // PRINCIPALS (Session::principal: "local" for every LOCAL session)
        // whose latch was ended by anything but its OWNING session's LATCH 0
        // - a timeout, a stop, a fault, a transmitter change or close, a lost
        // keep-alive, a closed session, a LATCH 0 from any session that did
        // not hold the latch (the operator's other window included) - with
        // why it ended. While a principal is marked, LATCH 1 from ANY of its
        // sessions - including sessions opened later - is refused, except
        // from a session in `exempt`: one that has itself sent a LATCH 0
        // since the mark was set (whether or not a latch was active then).
        // A new marking end re-marks and empties `exempt`. The mark itself
        // is never cleared, and closing sessions never exempts anyone.
        // Kept per principal, not per session (round-4 review: the mark died
        // with its session, so a window that was closed for stalling and
        // reconnected latched again); every kind of end marks (round-3
        // review); nothing another principal does frees one (round-2
        // review); and a LATCH 0 exempts ONLY the session that sent it
        // (round-5 review: when any session's LATCH 0 with nothing latched
        // cleared the mark, the operator's RELEASE in a second window freed
        // the stuck one, and merging the LATCH 0s of a stalled pass changed
        // which way it went). So LATCH 0 is idempotent: sending it once or
        // five times, early or late, leaves the same state.
        struct LatchMark {
            std::string reason;
            std::set<uint64_t> exempt;
        };
        std::map<std::string, LatchMark> latchMarks;
        int64_t latchEndedMs = 0;  // when the last latch ended, for any reason; 0 = never
        std::string latchEndReason;  // ...and why, for the re-arm refusal
        int64_t latchUntilMs = 0;
        std::string unkeyReason;
        // Change counters.
        uint64_t seq = 1, tuneSeq = 1, modeSeq = 1, deviceSeq = 1, audioSeq = 1,
                 displaySeq = 1, txSeq = 1, listSeq = 1;
    };

    enum Group : uint32_t {
        kTune = 1u << 0, kMode = 1u << 1, kDevice = 1u << 2, kAudio = 1u << 3,
        kDisplay = 1u << 4, kTx = 1u << 5, kList = 1u << 6,
    };

    static bool finite(const FoxCommand& c) {
        for (double d : c.num) {
            if (!std::isfinite(d)) {
                return false;
            }
        }
        return true;
    }

    // TX_PTT 0, TX_LATCH 0, RUN 0, TX_CLOSE, TX_REMOTE_ARM 0: each can only
    // open the key, stop, close or withdraw consent - never close the key or
    // configure anything. Returns its kind (0..kSaferKinds-1), else -1.
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

    // Queues one command (queueMutex_ and the sender's `m` held). EVERY send
    // keeps its own place in the queue - a merged safer command is applied
    // where it was sent, exactly as if it had not been merged - with one
    // exception, which is what keeps the queue bounded: a MERGED send is
    // dropped when the same session's same safer kind is already queued and
    // everything queued since is a (valid) safer command, from any session.
    // Such a send cannot change anything: safer commands never close the key,
    // latch, start, open a transmitter or give consent, so once a kind has
    // been applied nothing between can give a second application anything to
    // do (the key it would open is open, the latch it would end has ended and
    // no latch can have closed since - so no new mark either, and the sender
    // is already exempt). "Sent once or N times, merged or not, the same
    // state" is therefore literally true (test_merge_exhaustive checks every
    // small case). Round-6 review: moving a merged copy to its LAST send's
    // place ran it after another window's own LATCH 0 and turned a marked end
    // into an unmarked one. A send with a NEW ticket is always queued (its
    // result must come); a safer command refused at submit is never a safer
    // command here (submit() gives it saferKind -1).
    void enqueueLocked(Session& s, Pending p) {
        if (p.saferKind < 0) {
            queue_.push_back(std::move(p));
            return;
        }
        if (p.merged) {
            for (auto it = queue_.rbegin(); it != queue_.rend() && it->saferKind >= 0; ++it) {
                if (it->sessionId == p.sessionId && it->saferKind == p.saferKind) {
                    return;  // a repeat that changes nothing: its ticket's result comes from the first
                }
            }
        }
        ++s.safer[static_cast<std::size_t>(p.saferKind)].copies;
        queue_.push_back(std::move(p));
    }

    static bool textTerminated(const FoxCommand& c) {
        return std::memchr(c.text, '\0', sizeof(c.text)) != nullptr;
    }

    static uint64_t grantFor(uint32_t op) {
        switch (op) {
        case FOXAPI_OP_SET_CENTRE: case FOXAPI_OP_SET_FREQUENCY:
        case FOXAPI_OP_SET_VFO_OFFSET: case FOXAPI_OP_STEP_TUNE:
        case FOXAPI_OP_BOOKMARK_TUNE:
            return FOXAPI_GRANT_TUNE;
        case FOXAPI_OP_TX_OPEN: case FOXAPI_OP_TX_CLOSE: case FOXAPI_OP_TX_PTT:
        case FOXAPI_OP_TX_LATCH: case FOXAPI_OP_TX_SET_MODE:
        case FOXAPI_OP_TX_SET_FREQUENCY: case FOXAPI_OP_TX_SET_POWER:
        case FOXAPI_OP_TX_REMOTE_ARM:
            return FOXAPI_GRANT_TRANSMIT;
        case FOXAPI_OP_SETTING_SET:
            return FOXAPI_GRANT_ADMIN;
        default:
            return FOXAPI_GRANT_SETTINGS;
        }
    }

    // Rule 2: validation and permission, answered at once on the caller's
    // thread. Reads only the published snapshot, never the model.
    int32_t validate(const Session& s, const FoxCommand& c, const Published* snap) const {
        if (!finite(c) || !textTerminated(c)) {
            return FOXAPI_BAD_ARGUMENT;
        }
        switch (c.op) {
        case FOXAPI_OP_RUN: case FOXAPI_OP_SET_CENTRE: case FOXAPI_OP_SET_FREQUENCY:
        case FOXAPI_OP_SET_VFO_OFFSET: case FOXAPI_OP_STEP_TUNE: case FOXAPI_OP_SET_MODE:
        case FOXAPI_OP_SET_BANDWIDTH: case FOXAPI_OP_SET_SQUELCH: case FOXAPI_OP_SET_VOLUME:
        case FOXAPI_OP_SET_MUTED: case FOXAPI_OP_SET_DEEMPHASIS: case FOXAPI_OP_SET_STEREO:
        case FOXAPI_OP_SET_NR: case FOXAPI_OP_SET_NOTCH: case FOXAPI_OP_SET_AUTO_NOTCH:
        case FOXAPI_OP_SET_DISPLAY_RANGE: case FOXAPI_OP_SCAN_DEVICES:
        case FOXAPI_OP_SELECT_SOURCE: case FOXAPI_OP_SET_SAMPLE_RATE: case FOXAPI_OP_SET_GAIN:
        case FOXAPI_OP_SET_DEVICE_AGC: case FOXAPI_OP_BOOKMARK_ADD: case FOXAPI_OP_BOOKMARK_TUNE:
        case FOXAPI_OP_BOOKMARK_REMOVE: case FOXAPI_OP_TX_OPEN: case FOXAPI_OP_TX_CLOSE:
        case FOXAPI_OP_TX_PTT: case FOXAPI_OP_TX_LATCH: case FOXAPI_OP_TX_SET_MODE:
        case FOXAPI_OP_TX_SET_FREQUENCY: case FOXAPI_OP_TX_SET_POWER: case FOXAPI_OP_SETTING_SET:
        case FOXAPI_OP_TX_REMOTE_ARM: case FOXAPI_OP_BOOKMARK_FAVOURITE:
        case FOXAPI_OP_BOOKMARK_REMOVE_GROUP:
            break;
        default:
            // A newer interface's op, or one this engine does not implement:
            // it degrades, it does not fail.
            return FOXAPI_UNSUPPORTED;
        }
        if ((s.grants & grantFor(c.op)) == 0u) {
            return FOXAPI_DENIED;
        }
        // A REMOTE SESSION HAS THE WEB REMOTE'S POWER AND NO MORE: of the
        // transmitter it may only press and release the PTT (the app's web
        // remote can send nothing but transmitPtt), and pressing it needs the
        // local operator's standing consent (TX_REMOTE_ARM, below).
        if ((s.flags & FOXAPI_SESSION_REMOTE) != 0u && grantFor(c.op) == FOXAPI_GRANT_TRANSMIT &&
            c.op != FOXAPI_OP_TX_PTT) {
            return FOXAPI_DENIED;
        }
        const double v = c.num[0];
        switch (c.op) {
        case FOXAPI_OP_SET_CENTRE:
            return (v > 0.0 && v <= kMaxCentreHz) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_FREQUENCY:
            return (v > 0.0 && v <= kMaxCentreHz) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_VFO_OFFSET:
            return std::fabs(v) < kMaxVfoOffsetHz ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_STEP_TUNE:
            return (v > 0.0 && v <= 1.0e9 && c.ival[0] != 0 && std::llabs(c.ival[0]) <= 1000000)
                       ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_MODE:
            return (c.ival[0] > 0 && findMode(static_cast<uint32_t>(c.ival[0])) != nullptr)
                       ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_BANDWIDTH:
            return (v >= kMinBandwidthHz && v <= kMaxBandwidthHz) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_SQUELCH:
            return (v >= kMinSquelchDb && v <= kMaxSquelchDb) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_VOLUME:
            return (v >= 0.0 && v <= 1.0) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_DEEMPHASIS:
            return (c.ival[0] >= 0 && c.ival[0] <= 2) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_NR:
            return (c.ival[1] == 0 || (v >= 0.0 && v <= 1.0)) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_NOTCH:
            return (c.ival[1] == 0 || (v >= 10.0 && v <= 20000.0 && c.num[1] >= 0.1 && c.num[1] <= 1000.0))
                       ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_DISPLAY_RANGE:
            return (v >= kMinDisplayDb && v <= kMaxDisplayDb && c.num[1] >= kMinDisplayDb &&
                    c.num[1] <= kMaxDisplayDb)
                       ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_SAMPLE_RATE:
            return (v >= 8000.0 && v <= 61.44e6) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_SET_GAIN:
            return (c.text[0] != '\0' && v >= -50.0 && v <= 200.0) ? FOXAPI_OK : FOXAPI_BAD_ARGUMENT;
        case FOXAPI_OP_BOOKMARK_ADD:
        case FOXAPI_OP_BOOKMARK_REMOVE_GROUP:
            return c.text[0] != '\0' ? FOXAPI_OK : FOXAPI_BAD_ARGUMENT;
        case FOXAPI_OP_TX_SET_MODE:
            return (c.ival[0] >= 0 && c.ival[0] <= FOXAPI_TX_MODE_LSB) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_TX_SET_FREQUENCY:
            return (v > 0.0 && v <= kMaxCentreHz) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_TX_SET_POWER:
            return (v <= 0.0 && v >= -89.75) ? FOXAPI_OK : FOXAPI_OUT_OF_RANGE;
        case FOXAPI_OP_TX_PTT:
            // THE KEY IS NOT SIMPLY QUEUED. A key request with no transmitter
            // behind it is refused now, so it cannot sit in the queue being
            // true while someone opens a radio underneath it.
            if (c.ival[0] != 0 &&
                (snap == nullptr || (snap->st.flags & FOXAPI_RX_TX_AVAILABLE) == 0u)) {
                return FOXAPI_NO_DEVICE;
            }
            if (c.ival[0] != 0 && (s.flags & FOXAPI_SESSION_REMOTE) != 0u &&
                (snap == nullptr || (snap->st.flags & FOXAPI_RX_TX_REMOTE_ARMED) == 0u)) {
                return FOXAPI_DENIED;  // no local consent to remote transmit
            }
            return FOXAPI_OK;
        case FOXAPI_OP_TX_LATCH:
            if (c.ival[0] != 0) {
                // A latch keeps a radio keyed with nobody touching anything;
                // at the far end of a network that is the ordinary state.
                if ((s.flags & FOXAPI_SESSION_LOCAL) == 0u) {
                    return FOXAPI_DENIED;
                }
                if (snap == nullptr || (snap->st.flags & FOXAPI_RX_TX_AVAILABLE) == 0u) {
                    return FOXAPI_NO_DEVICE;
                }
            }
            return FOXAPI_OK;
        case FOXAPI_OP_SETTING_SET:
            return std::strchr(c.text, '=') != nullptr ? FOXAPI_OK : FOXAPI_BAD_ARGUMENT;
        default:
            return FOXAPI_OK;
        }
    }

    // ---- the control thread -------------------------------------------------

    void controlLoop() {
        const auto period = std::chrono::microseconds(1000000 / opt_.controlHz);
        while (true) {
            std::vector<Pending> work;
            std::vector<uint64_t> closed;
            {
                std::unique_lock<std::mutex> lk(queueMutex_);
                if (gather_ > 0) {
                    // Test seam (mock.gather): this pass takes the queue only
                    // once `gather_` more commands have been submitted (at
                    // most 2 s), so a test can put a batch from several
                    // sessions into ONE pass without timing anything.
                    const uint64_t want = gatherFrom_ + gather_;
                    queueCv_.wait_for(lk, std::chrono::seconds(2), [this, want] {
                        return submitted_ >= want || !run_.load();
                    });
                    gather_ = 0;
                } else {
                    queueCv_.wait_for(lk, period, [this] {
                        return !queue_.empty() || !closedSessions_.empty() || !run_.load();
                    });
                }
                if (!run_.load()) {
                    break;
                }
                work.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
                queue_.clear();
                closed.swap(closedSessions_);
            }
            const Model before = m_;
            // Sessions closed or detached since the last pass are dealt with
            // FIRST, before any command is applied - so nothing they queued
            // can key the transmitter even for one pass (round-3 review), and
            // apply() refuses everything but the safer commands they left in
            // the queue (round-4 review). Their latch MARKS stay: a mark
            // belongs to the principal, and closing a session never clears it
            // (round-4 review: a reconnecting window escaped its own mark).
            // Only the gone session's own exemption goes with it (session ids
            // are never reused, so this just keeps the sets bounded).
            for (uint64_t id : closed) {
                for (auto& mark : m_.latchMarks) {
                    mark.second.exempt.erase(id);
                }
                const bool detached = sessionFate(id) == SessionFate::kDetached;
                if (m_.keyOwner == id) {
                    unkey(detached ? "the login of the interface holding the key was revoked"
                                   : "the interface holding the key closed its session");
                }
                if (m_.remoteArmedBy == id) {
                    disarmRemote(detached ? "the login of the local interface that gave consent was revoked"
                                          : "the local interface that gave consent to remote transmit closed");
                }
            }
            // In queue order, each where it was sent (enqueueLocked).
            for (const Pending& p : work) {
                apply(p);
            }
            // A detached session is closed by the engine now that what it
            // left queued has been dealt with (round-4 review: detached
            // sessions held places until their client closed them, and
            // remote logins could fill all of them). Its handle stays valid,
            // answering DETACHED, until close_session.
            reapDetached(closed);
            const int64_t now = steadyMs();
            expireTokens();
            {
                const std::string ks = keyStateText();
                std::lock_guard<std::mutex> lk(settingsMutex_);
                keyState_ = ks;
            }
            tickTransmitter(now);
            publishReleaseFirst();
            publishParams();
            publishState();
            // Results go out only now, AFTER the state they describe has been
            // published: a client that reads "OK" and then the snapshot sees
            // what the command did. (Delivered inside apply(), a result could
            // be read before this pass's publish, and a LATCH 1 answered OK
            // read back unkeyed - a race the round-4 break-it run exposed.)
            // And in TICKET order per session (API.md rule 2): a merged
            // safer command is applied in the place of its LAST send, which
            // can be behind later tickets of the same session (round-5
            // review: RUN 0 t1, VOLUME t2, RUN 0 merged into t1 came out 2
            // then 1). Every result of a pass is delivered before the next
            // pass, and a merged copy only ever moves behind commands queued
            // in the same pass, so ordering each pass's results by ticket
            // keeps each session's results in ticket order for good. Stable,
            // so two applications of one ticket keep their order and the
            // later one is the one the result reports.
            std::stable_sort(outbox_.begin(), outbox_.end(), [](const Outgoing& x, const Outgoing& y) {
                return x.sessionId != y.sessionId ? x.sessionId < y.sessionId
                                                  : x.result.ticket < y.result.ticket;
            });
            for (const Outgoing& o : outbox_) {
                deliverResult(o.sessionId, o.saferKind, o.result);
            }
            outbox_.clear();
            const uint32_t moved = groupsMoved(before);
            if (moved != 0u) {
                FoxEvent e{};
                e.kind = FOXAPI_EVENT_STATE;
                e.value[0] = moved;
                broadcast(e);
            }
            if (stallAfterPassMs_ > 0) {  // test seam: mock.stallAfterPass
                const int ms = stallAfterPassMs_;
                stallAfterPassMs_ = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
        }
        // Shutting down: nothing stays keyed past the engine.
        if (m_.keyOwner != 0) {
            unkey("the engine is shutting down");
        }
    }

    uint32_t groupsMoved(const Model& b) const {
        uint32_t g = 0;
        if (b.tuneSeq != m_.tuneSeq) g |= kTune;
        if (b.modeSeq != m_.modeSeq) g |= kMode;
        if (b.deviceSeq != m_.deviceSeq) g |= kDevice;
        if (b.audioSeq != m_.audioSeq) g |= kAudio;
        if (b.displaySeq != m_.displaySeq) g |= kDisplay;
        if (b.txSeq != m_.txSeq) g |= kTx;
        if (b.listSeq != m_.listSeq) g |= kList;
        return g;
    }

    void bump(uint32_t groups) {
        ++m_.seq;
        if (groups & kTune) ++m_.tuneSeq;
        if (groups & kMode) ++m_.modeSeq;
        if (groups & kDevice) ++m_.deviceSeq;
        if (groups & kAudio) ++m_.audioSeq;
        if (groups & kDisplay) ++m_.displaySeq;
        if (groups & kTx) ++m_.txSeq;
        if (groups & kList) ++m_.listSeq;
    }

    double channelRateHz() const {
        const double decim = std::max(1.0, std::round(m_.sampleRateHz / 200000.0));
        return m_.sampleRateHz / decim;
    }

    // Keeps the channel inside the sampled band.
    double clampOffset(double off) const {
        const double lim = 0.5 * m_.sampleRateHz - 0.5 * m_.bandwidthHz;
        return lim > 0.0 ? std::clamp(off, -lim, lim) : 0.0;
    }

    // Puts the tuned channel on `tunedHz`: the VFO offset is kept and the
    // radio's centre follows, exactly as the counter and set_frequency do.
    void tuneTo(double tunedHz) {
        m_.centreHz = std::clamp(tunedHz - m_.vfoOffsetHz, 1.0, kMaxCentreHz);
    }

    // Applies one queued command.
    void apply(const Pending& p) {
        const FoxCommand& c = p.cmd;
        FoxCommandResult r{};
        r.structSize = sizeof(r);
        r.ticket = p.ticket;
        r.op = c.op;
        r.status = FOXAPI_OK;
        auto fail = [&r](int32_t st, const char* why) {
            r.status = st;
            copyText(r.message, why);
        };
        auto finish = [&] {
            outbox_.push_back({p.sessionId, p.saferKind, r});  // sent after this pass publishes
        };
        if (p.refused != FOXAPI_OK) {
            // Refused at submit: nothing is applied, and the answer arrives
            // here, in order, like every other result.
            r.status = p.refused;
            r.flags |= FOXAPI_RESULT_REFUSED;
            copyText(r.message, refusalText(p));
            finish();
            return;
        }
        // The session may have gone since it queued this: detached (its login
        // was revoked or expired) or closed, perhaps earlier in this very
        // pass. Only what makes the transmitter SAFER is applied for a
        // session that has gone - a stop or a release sent just before
        // closing must still happen - and everything else it left queued is
        // refused. (Round-4 review: a closed window's queued TX_REMOTE_ARM 1
        // was applied, and a remote PTT queued behind it keyed on consent
        // from a window that no longer existed.)
        const SessionFate fate = sessionFate(p.sessionId);
        if (fate != SessionFate::kOpen && !onlyMakesSafer(c)) {
            r.status = FOXAPI_DETACHED;
            copyText(r.message, fate == SessionFate::kDetached
                                    ? "the session was detached before this command was applied"
                                    : "the session closed before this command was applied");
            finish();  // nobody reads it; kept for the invariant
            return;
        }
        const double v = c.num[0];
        switch (c.op) {
        case FOXAPI_OP_RUN:
            m_.running = c.ival[0] != 0;
            if (!m_.running && m_.keyOwner != 0) {
                unkey("the receiver was stopped");  // the key never survives a stop
            }
            if (m_.running) {
                m_.faulted = false;  // a fault latches until the next start
                m_.faultMessage.clear();
            }
            r.applied[0] = m_.running ? 1.0 : 0.0;
            bump(kDevice);
            break;
        case FOXAPI_OP_SET_CENTRE:
            m_.centreHz = v;
            r.applied[0] = m_.centreHz;
            bump(kTune);
            break;
        case FOXAPI_OP_SET_FREQUENCY:
            tuneTo(v);
            r.applied[0] = m_.centreHz + m_.vfoOffsetHz;
            if (r.applied[0] != v) r.flags |= FOXAPI_RESULT_CLAMPED;
            bump(kTune);
            break;
        case FOXAPI_OP_SET_VFO_OFFSET:
            m_.vfoOffsetHz = clampOffset(v);
            r.applied[0] = m_.vfoOffsetHz;
            if (m_.vfoOffsetHz != v) r.flags |= FOXAPI_RESULT_CLAMPED;
            bump(kTune);
            break;
        case FOXAPI_OP_STEP_TUNE: {
            const double want = m_.centreHz + m_.vfoOffsetHz + static_cast<double>(c.ival[0]) * v;
            tuneTo(want);
            r.applied[0] = m_.centreHz + m_.vfoOffsetHz;
            if (r.applied[0] != want) r.flags |= FOXAPI_RESULT_CLAMPED;
            bump(kTune);
            break;
        }
        case FOXAPI_OP_SET_MODE: {
            const ModeInfo* mi = findMode(static_cast<uint32_t>(c.ival[0]));
            m_.mode = mi->mode;
            // A mode key moves the bandwidth to that mode's default.
            m_.bandwidthHz = std::min(mi->defaultBandwidthHz, 0.9 * channelRateHz());
            m_.vfoOffsetHz = clampOffset(m_.vfoOffsetHz);
            r.applied[0] = m_.mode;
            r.applied[1] = m_.bandwidthHz;
            bump(kMode | kTune);
            break;
        }
        case FOXAPI_OP_SET_BANDWIDTH: {
            const double hi = 0.9 * channelRateHz();
            m_.bandwidthHz = std::clamp(v, kMinBandwidthHz, hi);
            m_.vfoOffsetHz = clampOffset(m_.vfoOffsetHz);
            r.applied[0] = m_.bandwidthHz;
            if (m_.bandwidthHz != v) r.flags |= FOXAPI_RESULT_CLAMPED;
            bump(kMode);
            break;
        }
        case FOXAPI_OP_SET_SQUELCH:
            m_.squelchDb = v;
            r.applied[0] = v;
            bump(kMode);
            break;
        case FOXAPI_OP_SET_VOLUME:
            m_.volume = v;
            r.applied[0] = v;
            bump(kAudio);
            break;
        case FOXAPI_OP_SET_MUTED:
            m_.muted = c.ival[0] != 0;
            r.applied[0] = m_.muted ? 1.0 : 0.0;
            bump(kAudio);
            break;
        case FOXAPI_OP_SET_DEEMPHASIS:
            m_.deemphasis = static_cast<uint32_t>(c.ival[0]);
            r.applied[0] = m_.deemphasis;
            bump(kMode);
            break;
        case FOXAPI_OP_SET_STEREO:
            m_.stereo = c.ival[0] != 0;
            bump(kMode);
            break;
        case FOXAPI_OP_SET_NR:
            m_.nr = c.ival[0] != 0;
            if (c.ival[1] != 0) m_.nrStrength = v;
            bump(kMode);
            break;
        case FOXAPI_OP_SET_NOTCH:
            m_.notch = c.ival[0] != 0;
            if (c.ival[1] != 0) {
                m_.notchHz = v;
                m_.notchQ = c.num[1];
            }
            bump(kMode);
            break;
        case FOXAPI_OP_SET_AUTO_NOTCH:
            m_.autoNotch = c.ival[0] != 0;
            bump(kMode);
            break;
        case FOXAPI_OP_SET_DISPLAY_RANGE: {
            double lo = v;
            double hi = c.num[1];
            if (lo > hi - kMinDbSpan) {
                lo = hi - kMinDbSpan;  // push back the low end; never invert
                if (lo < kMinDisplayDb) {
                    lo = kMinDisplayDb;
                    hi = lo + kMinDbSpan;
                }
                r.flags |= FOXAPI_RESULT_CLAMPED;
            }
            m_.dbMin = lo;
            m_.dbMax = hi;
            r.applied[0] = lo;
            r.applied[1] = hi;
            bump(kDisplay);
            break;
        }
        case FOXAPI_OP_SCAN_DEVICES: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            scanned_ = true;
            bump(kList);
            break;
        }
        case FOXAPI_OP_SELECT_SOURCE: {
            // Matched against the LISTED ids, never passed to a driver as-is.
            int found = -1;
            {
                std::lock_guard<std::mutex> lk(listsMutex_);
                const int visible = scanned_ ? static_cast<int>(devices().size()) : 1;
                for (int i = 0; i < visible; ++i) {
                    if (std::strcmp(devices()[static_cast<std::size_t>(i)].id, c.text) == 0) {
                        found = i;
                    }
                }
                if (found >= 0) {
                    deviceIndexListed_ = found;
                }
            }
            if (found < 0) {
                fail(FOXAPI_NOT_FOUND, "no such device in the scanned list");
                break;
            }
            m_.deviceIndex = found;
            m_.sampleRateHz = 2048000.0;
            m_.vfoOffsetHz = clampOffset(m_.vfoOffsetHz);
            bump(kDevice | kList | kTune);
            break;
        }
        case FOXAPI_OP_SET_SAMPLE_RATE: {
            if (m_.deviceIndex == 0) {
                fail(FOXAPI_NO_DEVICE, "the signal generator runs at a fixed rate");
                break;
            }
            const auto& rates = devices()[static_cast<std::size_t>(m_.deviceIndex)].rates;
            if (std::find(rates.begin(), rates.end(), v) == rates.end()) {
                fail(FOXAPI_OUT_OF_RANGE, "this radio does not offer that rate");
                break;
            }
            m_.sampleRateHz = v;
            m_.bandwidthHz = std::min(m_.bandwidthHz, 0.9 * channelRateHz());
            m_.vfoOffsetHz = clampOffset(m_.vfoOffsetHz);
            r.applied[0] = v;
            bump(kDevice | kTune | kMode);
            break;
        }
        case FOXAPI_OP_SET_GAIN: {
            if (!devices()[static_cast<std::size_t>(m_.deviceIndex)].hasGain) {
                fail(FOXAPI_NO_DEVICE, "the signal generator has no gain stages");
                break;
            }
            if (std::strcmp(c.text, "TUNER") != 0) {
                fail(FOXAPI_NOT_FOUND, "no gain stage by that name");
                break;
            }
            m_.gainDb = std::clamp(std::round(v * 10.0) / 10.0, 0.0, 49.6);
            {
                std::lock_guard<std::mutex> lk(listsMutex_);
                gainListed_ = m_.gainDb;
            }
            r.applied[0] = m_.gainDb;
            if (m_.gainDb != v) r.flags |= FOXAPI_RESULT_CLAMPED;
            bump(kDevice | kList);
            break;
        }
        case FOXAPI_OP_SET_DEVICE_AGC:
            if (!devices()[static_cast<std::size_t>(m_.deviceIndex)].hasGain) {
                fail(FOXAPI_UNSUPPORTED, "the signal generator has no AGC");
                break;
            }
            m_.deviceAgc = c.ival[0] != 0;
            bump(kDevice);
            break;
        case FOXAPI_OP_BOOKMARK_ADD: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            if (bookmarks_.size() >= kMaxBookmarks) {
                fail(FOXAPI_LIMIT, "the bookmark list is full");
                break;
            }
            // As FreqManager::add: the current frequency, mode and bandwidth,
            // and a name already in use gets " (2)", " (3)", ...
            Bookmark b{++nextBookmarkId_, uniqueName(c.text), m_.centreHz + m_.vfoOffsetHz,
                       m_.mode, m_.bandwidthHz, "", false};
            insertSorted(b);
            r.applied[0] = static_cast<double>(b.id);
            bump(kList);
            break;
        }
        case FOXAPI_OP_BOOKMARK_FAVOURITE: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            Bookmark* b = findBookmark(static_cast<uint64_t>(c.ival[0]));
            if (b == nullptr) {
                fail(FOXAPI_NOT_FOUND, "no bookmark with that id");
                break;
            }
            b->favourite = c.ival[1] != 0;
            r.applied[0] = b->favourite ? 1.0 : 0.0;
            bump(kList);
            break;
        }
        case FOXAPI_OP_BOOKMARK_REMOVE_GROUP: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            const std::size_t before = bookmarks_.size();
            const std::string group = c.text;
            bookmarks_.erase(std::remove_if(bookmarks_.begin(), bookmarks_.end(),
                                            [&](const Bookmark& b) { return b.group == group; }),
                             bookmarks_.end());
            const std::size_t removed = before - bookmarks_.size();
            if (removed == 0) {
                fail(FOXAPI_NOT_FOUND, "no bookmark is in that group");
                break;
            }
            r.applied[0] = static_cast<double>(removed);
            bump(kList);
            break;
        }
        case FOXAPI_OP_BOOKMARK_TUNE:
        case FOXAPI_OP_BOOKMARK_REMOVE: {
            std::lock_guard<std::mutex> lk(listsMutex_);
            auto it = std::find_if(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark& b) {
                return b.id == static_cast<uint64_t>(c.ival[0]);
            });
            if (it == bookmarks_.end()) {
                fail(FOXAPI_NOT_FOUND, "no bookmark with that id");
                break;
            }
            if (c.op == FOXAPI_OP_BOOKMARK_TUNE) {
                // Frequency, mode AND bandwidth, as the app's bookmark tune.
                tuneTo(it->hz);
                m_.mode = it->mode;
                m_.bandwidthHz = std::clamp(it->bandwidthHz, kMinBandwidthHz, 0.9 * channelRateHz());
                m_.vfoOffsetHz = clampOffset(m_.vfoOffsetHz);
                tuneTo(it->hz);
                r.applied[0] = m_.centreHz + m_.vfoOffsetHz;
                r.applied[1] = m_.bandwidthHz;
                bump(kTune | kMode);
            } else {
                bookmarks_.erase(it);
                bump(kList);
            }
            break;
        }
        case FOXAPI_OP_TX_OPEN: {
            bool ok = false;
            for (const auto& d : kTxDevices) {
                ok = ok || std::strcmp(d.id, c.text) == 0;
            }
            if (!ok) {
                fail(FOXAPI_NOT_FOUND, "no such transmitter");
                break;
            }
            if (m_.keyOwner != 0) {
                unkey("the transmitter was changed");
            }
            m_.txOpen = true;
            bump(kTx);
            break;
        }
        case FOXAPI_OP_TX_CLOSE:
            if (m_.keyOwner != 0) {
                unkey("the transmitter was closed");
            }
            m_.txOpen = false;
            bump(kTx);
            break;
        case FOXAPI_OP_TX_PTT:
        case FOXAPI_OP_TX_LATCH:
            applyKey(p, r, fate == SessionFate::kOpen);
            break;
        case FOXAPI_OP_TX_REMOTE_ARM:
            if (c.ival[0] != 0) {
                // The consent belongs to THIS local session: it lapses when
                // the session disarms, closes or stops answering, as the
                // app's remote key lapses when the Transmit page closes.
                m_.remoteArmedBy = p.sessionId;
                bump(kTx);
            } else {
                disarmRemote("the local operator withdrew consent to remote transmit");
            }
            r.applied[0] = m_.remoteArmedBy != 0 ? 1.0 : 0.0;
            break;
        case FOXAPI_OP_TX_SET_MODE:
            m_.txMode = static_cast<uint32_t>(c.ival[0]);
            bump(kTx);
            break;
        case FOXAPI_OP_TX_SET_FREQUENCY:
            m_.txFrequencyHz = v;
            bump(kTx);
            break;
        case FOXAPI_OP_TX_SET_POWER:
            m_.txPowerDb = v;
            bump(kTx);
            break;
        case FOXAPI_OP_SETTING_SET: {
            const char* eq = std::strchr(c.text, '=');
            const std::string key(c.text, eq);
            const std::string val(eq + 1);
            if (key == "mock.bookmark") {
                // Test hook standing in for an import: "name|hz|mode|bw|group".
                std::vector<std::string> f;
                std::size_t a = 0;
                while (true) {
                    const std::size_t bar = val.find('|', a);
                    f.push_back(val.substr(a, bar == std::string::npos ? std::string::npos : bar - a));
                    if (bar == std::string::npos) break;
                    a = bar + 1;
                }
                if (f.size() != 5) {
                    fail(FOXAPI_BAD_ARGUMENT, "mock.bookmark is name|hz|mode|bw|group");
                    break;
                }
                std::lock_guard<std::mutex> lk(listsMutex_);
                if (bookmarks_.size() >= kMaxBookmarks) {
                    fail(FOXAPI_LIMIT, "the bookmark list is full");
                    break;
                }
                Bookmark b{++nextBookmarkId_, f[0], std::atof(f[1].c_str()),
                           static_cast<uint32_t>(std::atoi(f[2].c_str())), std::atof(f[3].c_str()),
                           f[4], false};
                insertSorted(b);
                r.applied[0] = static_cast<double>(b.id);
                bump(kList);
            } else if (key == "mock.fault") {
                // Test hook: a worker "dies". The receiver stops and the key
                // opens, exactly as a USB radio pulled mid-stream would do.
                m_.faulted = !val.empty();
                m_.faultMessage = val;
                if (m_.faulted) {
                    m_.running = false;
                    if (m_.keyOwner != 0) {
                        unkey("a fault stopped the engine");
                    }
                    FoxEvent e{};
                    e.kind = FOXAPI_EVENT_FAULT;
                    copyText(e.text, val.c_str());
                    broadcast(e);
                }
                bump(kDevice);
            } else if (key == "mock.openDelay") {
                // Test seam: open_session waits this long (at most a second)
                // between checking a token and listing the session.
                openDelayMs_.store(std::clamp(std::atoi(val.c_str()), 0, 1000));
            } else if (key == "mock.stall") {
                // Test hook: the control thread is busy for this long (at
                // most a second), so a test can queue commands and change a
                // session behind them before they are applied.
                const int ms = std::clamp(std::atoi(val.c_str()), 0, 1000);
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            } else if (key == "mock.stallAfterPass") {
                // Test hook: the control thread finishes this pass - its
                // keep-alive and token checks included - and THEN is busy for
                // this long (at most a second), so a test can close or detach
                // a session between a pass's checks and the next pass.
                stallAfterPassMs_ = std::clamp(std::atoi(val.c_str()), 0, 1000);
            } else if (key == "mock.gather") {
                // Test seam: the NEXT pass waits for this many commands to be
                // submitted (1..64), then takes them all at once.
                std::lock_guard<std::mutex> lk(queueMutex_);
                gatherFrom_ = submitted_;
                gather_ = static_cast<uint64_t>(std::clamp(std::atoi(val.c_str()), 1, 64));
            } else {
                std::lock_guard<std::mutex> lk(settingsMutex_);
                settings_[key] = val;
            }
            break;
        }
        default:
            fail(FOXAPI_UNSUPPORTED, "not implemented by the mock engine");
            break;
        }
        finish();
    }

    // ---- THE KEY -------------------------------------------------------------

    // `open`: the sending session is still open (not closed or detached).
    void applyKey(const Pending& p, FoxCommandResult& r, bool open) {
        const FoxCommand& c = p.cmd;
        const int64_t now = steadyMs();
        const bool on = c.ival[0] != 0;
        if (!on) {
            // Anyone with the TRANSMIT grant may OPEN the key; that is never
            // the unsafe direction.
            if (c.op == FOXAPI_OP_TX_PTT && m_.holdUntilMs != 0) {
                m_.holdUntilMs = 0;
            }
            if (c.op == FOXAPI_OP_TX_LATCH) {
                if (m_.latched) {
                    m_.latchEndedMs = now;  // the re-arm time starts now
                    if (m_.keyOwner == p.sessionId) {
                        // The owner's own release: no mark.
                        m_.latchEndReason = "released by its owner (LATCH 0)";
                    } else {
                        // Released from an interface that does NOT hold the
                        // latch - typically the operator pressing RELEASE in
                        // another window because the one holding it is
                        // stuck. The owner's principal is marked, so that
                        // window cannot latch again after the re-arm time:
                        // when in doubt the transmitter stays unkeyed.
                        m_.latchEndReason = m_.keyOwnerPrincipal == p.principal
                                                ? "released from another interface of the same operator (LATCH 0)"
                                                : "another interface released it (LATCH 0)";
                        markPrincipal(m_.keyOwnerPrincipal, m_.latchEndReason);
                    }
                }
                m_.latched = false;
                m_.latchUntilMs = 0;
                // Whatever it found, a LATCH 0 exempts THE SESSION THAT SENT
                // IT, and no other, from its principal's mark: the deliberate
                // release before pressing again works in the window where it
                // is pressed, while a stuck window - which never sends
                // LATCH 0 - and a reconnecting one stay refused. A session
                // that has gone exempts nobody: its release still releases
                // (above), but it can never press again.
                if (open) {
                    auto mark = m_.latchMarks.find(p.principal);
                    if (mark != m_.latchMarks.end()) {
                        mark->second.exempt.insert(p.sessionId);
                    }
                }
            }
            if (m_.holdUntilMs == 0 && !m_.latched && m_.keyOwner != 0) {
                m_.keyOwner = 0;
                m_.keyOwnerRemote = false;
                m_.keyOwnerPrincipal.clear();
                m_.unkeyReason.clear();  // the operator opened it themselves
            }
            bump(kTx);
            return;
        }
        if (!m_.txOpen) {
            r.status = FOXAPI_NO_DEVICE;
            copyText(r.message, "there is no transmitter open");
            return;
        }
        if (m_.keyOwner != 0 && m_.keyOwner != p.sessionId) {
            r.status = FOXAPI_BUSY;
            copyText(r.message, "another interface holds the key");
            return;
        }
        const bool remote = (p.sessionFlags & FOXAPI_SESSION_REMOTE) != 0u;
        if (remote && m_.remoteArmedBy == 0) {
            // Checked again here: consent may have been withdrawn since submit.
            r.status = FOXAPI_DENIED;
            copyText(r.message, "the local operator has not given consent to remote transmit");
            return;
        }
        if (c.op == FOXAPI_OP_TX_LATCH && m_.latched) {
            // A re-latch while latched changes nothing: the latch keeps its
            // ORIGINAL deadline (the app's setLatched(true) is a no-op while
            // latched), so no sequence of re-latches can hold it for ever.
            r.status = FOXAPI_NO_CHANGE;
            r.applied[0] = static_cast<double>(std::max<int64_t>(0, m_.latchUntilMs - now));
            copyText(r.message, "already latched; the latch keeps its original deadline");
            return;
        }
        if (c.op == FOXAPI_OP_TX_LATCH && m_.latchEndedMs != 0 &&
            now - m_.latchEndedMs < FOXAPI_LATCH_REARM_MS) {
            // THE KEY IS OFF FOR AT LEAST FOXAPI_LATCH_REARM_MS after any
            // latch ends, whoever asks: otherwise LATCH 0 + LATCH 1 in one
            // batch every frame re-closes it in the same control pass and it
            // never opens (round-2 review: 1,494 ms keyed against 400).
            r.status = FOXAPI_DENIED;
            r.applied[0] = static_cast<double>(FOXAPI_LATCH_REARM_MS - (now - m_.latchEndedMs));
            const std::string msg = "no latch closes within " + std::to_string(FOXAPI_LATCH_REARM_MS) +
                                    " ms of the last one ending (" + m_.latchEndReason + ")";
            copyText(r.message, msg.c_str());
            return;
        }
        if (c.op == FOXAPI_OP_TX_LATCH) {
            auto mark = m_.latchMarks.find(p.principal);
            if (mark != m_.latchMarks.end() && mark->second.exempt.count(p.sessionId) == 0) {
                // This operator's last latch was ended by something other
                // than its owner's own LATCH 0, and THIS interface has not
                // released since. It must send LATCH 0 itself before
                // latching again, as the operator must see the latch key go
                // out before pressing it again: otherwise an interface that
                // re-sends LATCH 1 every frame re-latches the moment the
                // re-arm time runs out after a stop, a fault or a stall - or
                // after it is closed and reconnects, or after the operator
                // presses RELEASE in another window - and holds the key in a
                // chain of latches.
                r.status = FOXAPI_DENIED;
                const std::string msg = "the latch ended (" + mark->second.reason +
                                        "): this interface must send LATCH 0 first";
                copyText(r.message, msg.c_str());
                return;
            }
        }
        m_.keyOwner = p.sessionId;
        m_.keyOwnerRemote = remote;
        m_.keyOwnerPrincipal = p.principal;
        if (c.op == FOXAPI_OP_TX_PTT) {
            m_.holdUntilMs = now + opt_.pttHoldMs;  // a hold, never a switch
        } else {
            m_.latched = true;
            m_.latchUntilMs = now + opt_.latchTimeoutMs;
        }
        r.applied[0] = 1.0;
        bump(kTx);
    }

    // Withdraws the local consent to remote PTT; a key a REMOTE session holds
    // opens with it.
    void disarmRemote(const char* why) {
        if (m_.remoteArmedBy == 0) {
            return;
        }
        m_.remoteArmedBy = 0;
        bump(kTx);
        if (m_.keyOwner != 0 && m_.keyOwnerRemote) {
            unkey(why);
        }
    }

    // FOXAPI_RX_TX_LATCH_RELEASE_FIRST, per session: its principal is marked
    // and it has not sent LATCH 0 since (the rule applyKey enforces). Once a
    // pass, before the state is published, so the flag trails the snapshot
    // by at most that pass.
    void publishReleaseFirst() {
        for (const auto& s : liveSessions()) {
            auto mark = m_.latchMarks.find(s->principal);
            s->releaseFirst.store(mark != m_.latchMarks.end() && mark->second.exempt.count(s->id) == 0,
                                  std::memory_order_relaxed);
        }
    }

    // Marks (or re-marks) a principal: every one of its sessions is refused
    // LATCH 1 until that session sends LATCH 0 (Model::latchMarks).
    void markPrincipal(const std::string& principal, const std::string& why) {
        if (principal.empty()) {
            return;
        }
        Model::LatchMark& mark = m_.latchMarks[principal];
        mark.reason = why;
        mark.exempt.clear();  // a new end: nobody has released since
    }

    // Opens the key on the ENGINE's account (never the owner's own release).
    // A latch it ends marks its owner's principal (see Model::latchMarks).
    void unkey(const char* why) {
        if (m_.latched) {
            m_.latchEndedMs = steadyMs();  // however it ended, the re-arm time starts
            m_.latchEndReason = why;
            markPrincipal(m_.keyOwnerPrincipal, why);
        }
        m_.keyOwner = 0;
        m_.keyOwnerRemote = false;
        m_.keyOwnerPrincipal.clear();
        m_.holdUntilMs = 0;
        m_.latched = false;
        m_.latchUntilMs = 0;
        m_.unkeyReason = why;
        bump(kTx);
        FoxEvent e{};
        e.kind = FOXAPI_EVENT_TX_UNKEYED;
        copyText(e.text, why);
        broadcast(e);
    }

    // Once per control pass: the hold, the latch and the owner's keep-alive.
    void tickTransmitter(int64_t now) {
        if (m_.remoteArmedBy != 0) {
            std::shared_ptr<Session> armer;
            {
                std::lock_guard<std::mutex> lk(sessionsMutex_);
                auto it = sessions_.find(m_.remoteArmedBy);
                if (it != sessions_.end()) {
                    armer = it->second;
                }
            }
            if (!armer) {
                disarmRemote("the local interface that gave consent to remote transmit closed");
            } else if (now - armer->lastBeatMs.load(std::memory_order_relaxed) > opt_.keepaliveMs) {
                disarmRemote("the local interface that gave consent to remote transmit stopped answering");
            }
        }
        if (m_.keyOwner == 0) {
            return;
        }
        if (m_.holdUntilMs != 0 && now >= m_.holdUntilMs) {
            m_.holdUntilMs = 0;
            if (!m_.latched) {
                unkey("the PTT was not re-asserted within the hold");
                return;
            }
        }
        if (m_.latched && now >= m_.latchUntilMs) {
            unkey("the latch timed out");  // marks the owner, as every engine-side end does
            return;
        }
        std::shared_ptr<Session> owner;
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            auto it = sessions_.find(m_.keyOwner);
            if (it != sessions_.end()) {
                owner = it->second;
            }
        }
        if (!owner || owner->detached.load()) {
            unkey(owner ? "the login of the interface holding the key was revoked"
                        : "the interface holding the key closed its session");
            return;
        }
        // A remote session's keep-alive is the web remote's hold (2 s), a
        // local one's the frame loop's dead-man (1 s).
        const int limit = (owner->flags & FOXAPI_SESSION_REMOTE) != 0u ? opt_.remoteKeepaliveMs
                                                                        : opt_.keepaliveMs;
        if (now - owner->lastBeatMs.load(std::memory_order_relaxed) > limit) {
            unkey("the interface holding the key stopped answering");
        }
    }

    // ---- bookmarks (listsMutex_ held) --------------------------------------------

    // Sorted by frequency, stable: equal frequencies keep insertion order.
    void insertSorted(const Bookmark& b) {
        auto at = std::upper_bound(bookmarks_.begin(), bookmarks_.end(), b.hz,
                                   [](double hz, const Bookmark& x) { return hz < x.hz; });
        bookmarks_.insert(at, b);
    }

    std::string uniqueName(const std::string& want) const {
        auto used = [&](const std::string& n) {
            return std::any_of(bookmarks_.begin(), bookmarks_.end(),
                               [&](const Bookmark& b) { return b.name == n; });
        };
        if (!used(want)) {
            return want;
        }
        for (int k = 2;; ++k) {
            const std::string n = want + " (" + std::to_string(k) + ")";
            if (!used(n)) {
                return n;
            }
        }
    }

    Bookmark* findBookmark(uint64_t id) {
        for (Bookmark& b : bookmarks_) {
            if (b.id == id) {
                return &b;
            }
        }
        return nullptr;
    }

    // A sentence for a command refused at submit.
    static const char* refusalText(const Pending& p) {
        const bool remote = (p.sessionFlags & FOXAPI_SESSION_REMOTE) != 0u;
        switch (p.refused) {
        case FOXAPI_DENIED:
            if (remote && (p.cmd.op & 0xFF00u) == 0x0A00u) {
                return p.cmd.op == FOXAPI_OP_TX_PTT
                           ? "the local operator has not given consent to remote transmit"
                           : "a remote session may only press and release the PTT";
            }
            if (remote && p.cmd.op == FOXAPI_OP_TX_LATCH) {
                return "the latch is a hands-on control and cannot be closed remotely";
            }
            return "this session does not hold the grant for that";
        case FOXAPI_OUT_OF_RANGE: return "a value is outside what the engine accepts";
        case FOXAPI_BAD_ARGUMENT: return "the command is malformed (NaN, unterminated or empty text)";
        case FOXAPI_UNSUPPORTED: return "this engine does not implement that command";
        case FOXAPI_NO_DEVICE: return "there is no transmitter open";
        default: return "refused";
        }
    }

    // ---- delivery -------------------------------------------------------------

    enum class SessionFate { kOpen, kDetached, kClosed };

    // Whether the session that queued a command is still there to act.
    SessionFate sessionFate(uint64_t id) {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return reaped_.count(id) != 0 ? SessionFate::kDetached : SessionFate::kClosed;
        }
        return it->second->detached.load() ? SessionFate::kDetached : SessionFate::kOpen;
    }

    // The engine closes a DETACHED session itself: it leaves the session
    // list (and the limits) and keeps only its handle, which answers
    // DETACHED until close_session frees it.
    void reapDetached(const std::vector<uint64_t>& ids) {
        std::vector<std::shared_ptr<Session>> gone;
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            for (uint64_t id : ids) {
                auto it = sessions_.find(id);
                if (it != sessions_.end() && it->second->detached.load()) {
                    gone.push_back(it->second);
                    reaped_[id] = it->second;
                    sessions_.erase(it);
                }
            }
        }
        for (auto& s : gone) {
            // Nobody can read them: every call answers DETACHED. Their memory
            // is freed now (swapped out, not just cleared), so what a reaped
            // handle keeps until close_session is the Session object itself
            // - small and of fixed size - which the engine cannot free while
            // the client may still call through the handle.
            std::lock_guard<std::mutex> lk(s->m);
            std::deque<FoxCommandResult>().swap(s->results);
            std::deque<FoxEvent>().swap(s->events);
        }
    }

    std::vector<std::shared_ptr<Session>> liveSessions() {
        std::lock_guard<std::mutex> lk(sessionsMutex_);
        std::vector<std::shared_ptr<Session>> v;
        v.reserve(sessions_.size());
        for (auto& kv : sessions_) {
            v.push_back(kv.second);
        }
        return v;
    }

    void deliverResult(uint64_t sessionId, int saferKind, const FoxCommandResult& r) {
        std::shared_ptr<Session> s;
        {
            std::lock_guard<std::mutex> lk(sessionsMutex_);
            auto it = sessions_.find(sessionId);
            if (it == sessions_.end()) {
                return;  // closed meanwhile: nobody to tell
            }
            s = it->second;
        }
        FoxCommandResult out = r;
        if (out.status != FOXAPI_OK) {
            // Rule 2: a result whose status is not OK changed NOTHING -
            // refused at submit, refused when applied (a timed-out latch, a
            // consent withdrawn meanwhile, another key holder, no
            // transmitter), failed, or already so (NO_CHANGE) - and says so.
            out.flags |= FOXAPI_RESULT_REFUSED;
        }
        std::lock_guard<std::mutex> lk(s->m);
        if (saferKind < 0) {
            // Bounded by FOXAPI_MAX_PENDING: the command still counts as
            // pending until poll_results reads this.
            s->results.push_back(out);
            return;
        }
        // A safer command: its kind's ONE result, whatever was merged into it.
        Session::SaferSlot& slot = s->safer[static_cast<std::size_t>(saferKind)];
        if (slot.ticket != r.ticket) {
            return;  // an older ticket of the kind, already answered and read
        }
        if (slot.copies > 0) {
            --slot.copies;
        }
        if (slot.read) {
            // Its result was read while this application was still queued:
            // it was answered then, and is not answered twice.
            if (slot.copies == 0) {
                slot = Session::SaferSlot{};
            }
            return;
        }
        if (slot.inResults) {
            for (FoxCommandResult& waiting : s->results) {
                if (waiting.ticket == r.ticket) {
                    waiting = out;  // the latest application, in the same place
                    break;
                }
            }
        } else {
            s->results.push_back(out);
            slot.inResults = true;
        }
    }

    void broadcast(FoxEvent e) {
        e.structSize = sizeof(e);
        e.unixMs = unixMs();
        for (auto& s : liveSessions()) {
            std::lock_guard<std::mutex> lk(s->m);
            if ((s->eventMask & FOXAPI_EVENT_MASK(e.kind)) == 0u) {
                continue;
            }
            if (s->events.size() >= kMaxEventsPerSession) {
                s->events.pop_front();
                ++s->eventsDropped;
            }
            e.seq = ++s->eventSeq;
            s->events.push_back(e);
        }
    }

    // ---- publishing -------------------------------------------------------------

    // The parameters the signal thread reads, as atomics: it never locks.
    void publishParams() {
        p_centreHz_.store(m_.centreHz, std::memory_order_relaxed);
        p_offsetHz_.store(m_.vfoOffsetHz, std::memory_order_relaxed);
        p_rateHz_.store(m_.sampleRateHz, std::memory_order_relaxed);
        p_bandwidthHz_.store(m_.bandwidthHz, std::memory_order_relaxed);
        p_squelchDb_.store(m_.squelchDb, std::memory_order_relaxed);
        p_volume_.store(m_.volume, std::memory_order_relaxed);
        p_muted_.store(m_.muted, std::memory_order_relaxed);
        p_mode_.store(m_.mode, std::memory_order_relaxed);
        p_gainDb_.store(m_.deviceIndex == 0 ? 0.0 : m_.gainDb - 29.7, std::memory_order_relaxed);
        p_running_.store(m_.running, std::memory_order_release);
    }

    void publishState() {
        Published p{};
        FoxReceiverState& s = p.st;
        s.structSize = sizeof(s);
        uint32_t f = 0;
        if (m_.running) f |= FOXAPI_RX_RUNNING;
        if (m_.deviceIndex != 0) f |= FOXAPI_RX_DEVICE_OPEN;
        if (m_.faulted) f |= FOXAPI_RX_FAULTED;
        if (m_.muted) f |= FOXAPI_RX_MUTED;
        const double sig = m_signalDb_.load(std::memory_order_relaxed);
        if (m_.running && sig > m_.squelchDb) f |= FOXAPI_RX_SQUELCH_OPEN;
        if (m_.stereo) f |= FOXAPI_RX_STEREO_ENABLED;
        if (m_.running && m_.stereo && m_.mode == FOXAPI_DEMOD_WFM && sig > -60.0) {
            f |= FOXAPI_RX_STEREO_ACTIVE;
        }
        if (m_.nr) f |= FOXAPI_RX_NR;
        if (m_.notch) f |= FOXAPI_RX_NOTCH;
        if (m_.autoNotch) f |= FOXAPI_RX_AUTO_NOTCH;
        if (m_.deviceAgc) f |= FOXAPI_RX_DEVICE_AGC;
        if (devices()[static_cast<std::size_t>(m_.deviceIndex)].hasGain) f |= FOXAPI_RX_AGC_SUPPORTED;
        if (m_.txOpen) f |= FOXAPI_RX_TX_AVAILABLE;
        if (m_.remoteArmedBy != 0) f |= FOXAPI_RX_TX_REMOTE_ARMED;
        if (m_.keyOwner != 0) f |= FOXAPI_RX_TX_KEYED;
        if (m_.latched) f |= FOXAPI_RX_TX_LATCHED;
        f |= FOXAPI_RX_SINK_OPEN;
        s.flags = f;
        s.seq = m_.seq;
        s.tuneSeq = m_.tuneSeq;
        s.modeSeq = m_.modeSeq;
        s.deviceSeq = m_.deviceSeq;
        s.audioSeq = m_.audioSeq;
        s.displaySeq = m_.displaySeq;
        s.txSeq = m_.txSeq;
        s.listSeq = m_.listSeq;
        s.centreHz = m_.centreHz;
        s.vfoOffsetHz = m_.vfoOffsetHz;
        s.tunedHz = m_.centreHz + m_.vfoOffsetHz;
        s.sampleRateHz = m_.sampleRateHz;
        s.channelRateHz = channelRateHz();
        s.bandwidthHz = m_.bandwidthHz;
        s.squelchDb = m_.squelchDb;
        s.volume = m_.volume;
        s.signalDb = sig;
        s.sMeter = std::clamp((sig + 120.0) / 120.0, 0.0, 1.0);
        s.audioLevelDb = m_audioDb_.load(std::memory_order_relaxed);
        s.dbMin = m_.dbMin;
        s.dbMax = m_.dbMax;
        s.nrStrength = m_.nrStrength;
        s.notchHz = m_.notchHz;
        s.notchQ = m_.notchQ;
        s.pilotLevel = (f & FOXAPI_RX_STEREO_ACTIVE) ? 1.0 : 0.0;
        s.demodMode = m_.mode;
        s.deemphasis = m_.deemphasis;
        s.gainCount = devices()[static_cast<std::size_t>(m_.deviceIndex)].hasGain ? 1u : 0u;
        s.decodersRunning = 0;
        s.decodersFitted = 0;
        s.txMode = m_.txMode;
        s.audioUnderruns = 0;
        s.txFrequencyHz = m_.txFrequencyHz;
        s.txPowerDb = m_.txPowerDb;
        const int64_t now = steadyMs();
        s.txHoldRemainingMs = m_.holdUntilMs != 0 ? std::max<int64_t>(0, m_.holdUntilMs - now) : 0;
        s.txLatchRemainingMs = m_.latched ? std::max<int64_t>(0, m_.latchUntilMs - now) : 0;
        copyText(s.deviceName, devices()[static_cast<std::size_t>(m_.deviceIndex)].name);
        copyText(s.sinkName, "Mock speakers (nothing is played)");
        copyText(s.faultMessage, m_.faultMessage.c_str());
        copyText(s.txUnkeyReason, m_.unkeyReason.c_str());
        p.keyOwner = m_.keyOwner;
        p.localMarked = m_.latchMarks.count("local") != 0;
        state_.store(p);
    }

    // ---- the signal thread (stands in for DSP) ---------------------------------

    void signalLoop() {
        const auto period = std::chrono::duration<double>(1.0 / opt_.fps);
        auto next = SteadyClock::now();
        auto last = next;
        std::vector<float> bins(static_cast<std::size_t>(opt_.bins), -120.0f);
        std::vector<float> smooth(static_cast<std::size_t>(opt_.bins), -100.0f);
        std::vector<float> audio;
        uint32_t rng = 0x9E3779B9u;
        auto rnd = [&rng]() {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return (rng & 0xFFFFFFu) / static_cast<double>(0x1000000);
        };
        double phase = 0.0;
        double phase2 = 0.0;
        double fade = 0.0;
        while (run_.load(std::memory_order_relaxed)) {
            next += std::chrono::duration_cast<SteadyClock::duration>(period);
            const auto now = SteadyClock::now();
            if (next < now - std::chrono::seconds(1)) {
                next = now;  // fell far behind (a debugger, a suspended laptop)
            }
            std::this_thread::sleep_until(next);
            const auto t = SteadyClock::now();
            const double dt = std::chrono::duration<double>(t - last).count();
            last = t;
            if (!p_running_.load(std::memory_order_acquire)) {
                m_audioDb_.store(-120.0, std::memory_order_relaxed);
                continue;
            }
            const double centre = p_centreHz_.load(std::memory_order_relaxed);
            const double rate = p_rateHz_.load(std::memory_order_relaxed);
            const double gain = p_gainDb_.load(std::memory_order_relaxed);
            fade += dt;
            const double floorLin = dbToLin(-100.0);
            const double lo = centre - 0.5 * rate;
            const double binHz = rate / static_cast<double>(opt_.bins);
            for (std::size_t i = 0; i < bins.size(); ++i) {
                const double f = lo + (static_cast<double>(i) + 0.5) * binHz;
                double lin = floorLin * (0.35 + 1.3 * rnd());
                for (const Station& s : stations_) {
                    const double off = f - s.hz;
                    if (std::fabs(off) > s.widthHz) {
                        continue;
                    }
                    const double wobble = 1.5 * std::sin(fade * 0.7 + s.hz * 1e-5);
                    lin += dbToLin(s.peakDb + wobble + gain) * humpShape(off, s.widthHz) *
                           (0.8 + 0.4 * rnd());
                }
                const float db = static_cast<float>(linToDb(lin));
                smooth[i] = 0.5f * smooth[i] + 0.5f * db;  // EMA, alpha 0.5
                bins[i] = smooth[i];
            }
            spectrum_.publish(bins, centre, rate, unixMs());

            // The channel: power of what falls inside the tuned bandwidth.
            const double tuned = centre + p_offsetHz_.load(std::memory_order_relaxed);
            const double bw = p_bandwidthHz_.load(std::memory_order_relaxed);
            double chanLin = floorLin * bw / std::max(binHz, 1.0) * 0.02;
            for (const Station& s : stations_) {
                const double a = std::max(tuned - 0.5 * bw, s.hz - 0.5 * s.widthHz);
                const double b = std::min(tuned + 0.5 * bw, s.hz + 0.5 * s.widthHz);
                if (b > a) {
                    chanLin += dbToLin(s.peakDb + gain) * (b - a) / s.widthHz;
                }
            }
            const double sigDb = linToDb(chanLin);
            m_signalDb_.store(sigDb, std::memory_order_relaxed);

            // Audio: a tone (two for WFM) scaled by how far the signal is
            // above the floor, gated by the squelch, scaled by the volume.
            const std::size_t n = static_cast<std::size_t>(kAudioRateHz * dt);
            audio.assign(n, 0.0f);
            const bool open = sigDb > p_squelchDb_.load(std::memory_order_relaxed);
            const bool muted = p_muted_.load(std::memory_order_relaxed);
            const double vol = p_volume_.load(std::memory_order_relaxed);
            double sumSq = 0.0;
            if (open && !muted && n > 0) {
                const double amp = vol * std::clamp((sigDb + 100.0) / 70.0, 0.0, 1.0) * 0.6;
                const bool wfm = p_mode_.load(std::memory_order_relaxed) == FOXAPI_DEMOD_WFM;
                for (std::size_t i = 0; i < n; ++i) {
                    phase += 2.0 * 3.14159265358979 * 440.0 / kAudioRateHz;
                    phase2 += 2.0 * 3.14159265358979 * 660.0 / kAudioRateHz;
                    double x = std::sin(phase);
                    if (wfm) {
                        x = 0.6 * x + 0.4 * std::sin(phase2);
                    }
                    x = amp * x + vol * 0.02 * (rnd() - 0.5);
                    audio[i] = static_cast<float>(x);
                    sumSq += x * x;
                }
                phase = std::fmod(phase, 2.0 * 3.14159265358979);
                phase2 = std::fmod(phase2, 2.0 * 3.14159265358979);
            }
            if (n > 0) {
                audio_.write(audio.data(), n);
            }
            const double rms = n > 0 ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0;
            m_audioDb_.store(std::max(-120.0, 20.0 * std::log10(rms + 1e-9)),
                             std::memory_order_relaxed);
        }
    }

    Options opt_;
    Model m_;  // control thread only
    // Results made in this control pass, delivered after it publishes.
    struct Outgoing {
        uint64_t sessionId;
        int saferKind;
        FoxCommandResult result;
    };
    std::vector<Outgoing> outbox_;  // control thread only

    // Parameter mirrors for the signal thread (written by the control thread).
    std::atomic<double> p_centreHz_{94.5e6};
    std::atomic<double> p_offsetHz_{0.0};
    std::atomic<double> p_rateHz_{2048000.0};
    std::atomic<double> p_bandwidthHz_{150000.0};
    std::atomic<double> p_squelchDb_{-120.0};
    std::atomic<double> p_volume_{0.5};
    std::atomic<bool> p_muted_{false};
    std::atomic<uint32_t> p_mode_{FOXAPI_DEMOD_WFM};
    std::atomic<double> p_gainDb_{0.0};
    std::atomic<bool> p_running_{false};
    // Measurements published by the signal thread.
    std::atomic<double> m_signalDb_{-200.0};
    std::atomic<double> m_audioDb_{-120.0};

    SpectrumRing spectrum_;
    foxsdr::AudioRing<1u << 17> audio_;  // 2.7 s at 48 kHz
    std::vector<Station> stations_;
    Seqlock<Published> state_;

    std::mutex sessionsMutex_;
    std::map<uint64_t, std::shared_ptr<Session>> sessions_;
    // Detached sessions the engine has closed: out of the list and the
    // limits, kept only until close_session frees the handle.
    std::map<uint64_t, std::shared_ptr<Session>> reaped_;
    uint64_t nextSessionId_ = 0;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Pending> queue_;
    std::vector<uint64_t> closedSessions_;

    // Lists readable from any thread, written by the control thread.
    std::mutex listsMutex_;
    bool scanned_ = false;
    int deviceIndexListed_ = 0;
    double gainListed_ = 29.7;
    std::vector<Bookmark> bookmarks_;
    uint64_t nextBookmarkId_ = 0;

    std::mutex settingsMutex_;
    std::map<std::string, std::string> settings_;

    std::mutex tokensMutex_;
    std::map<std::string, int64_t> tokens_;  // login tokens and their expiry (steady ms)
    int loginFailures_ = 0;
    int64_t lockedUntilMs_ = 0;

    std::atomic<int> openDelayMs_{0};  // test seam: mock.openDelay
    int stallAfterPassMs_ = 0;         // test seam: mock.stallAfterPass (control thread only)
    uint64_t submitted_ = 0;           // commands submit() has taken (queueMutex_)
    uint64_t gather_ = 0;              // test seam: mock.gather (control thread, under queueMutex_)
    uint64_t gatherFrom_ = 0;
    std::string keyState_;             // test seam: mock.keyState (settingsMutex_)

    // Test seam: everything the key-safety rules keep, as text - who holds
    // the key, the latch and the hold, running, the transmitter, consent,
    // why the key and the last latch ended, and each principal's mark with
    // its exempt sessions. Two runs that must end in the same state must
    // produce the same text. (Times are left out: they differ run to run.)
    std::string keyStateText() const {
        std::string s = "owner=" + std::to_string(m_.keyOwner) + " latched=" + std::to_string(m_.latched ? 1 : 0) +
                        " hold=" + std::to_string(m_.holdUntilMs != 0 ? 1 : 0) +
                        " running=" + std::to_string(m_.running ? 1 : 0) + " tx=" + std::to_string(m_.txOpen ? 1 : 0) +
                        " armedBy=" + std::to_string(m_.remoteArmedBy) + " ended=" +
                        std::to_string(m_.latchEndedMs != 0 ? 1 : 0) + " endReason=[" + m_.latchEndReason +
                        "] unkey=[" + m_.unkeyReason + "] marks={";
        for (const auto& kv : m_.latchMarks) {
            s += kv.first + ":[" + kv.second.reason + "]:(";
            for (uint64_t id : kv.second.exempt) {  // std::set: sorted
                s += std::to_string(id) + ",";
            }
            s += ") ";
        }
        return s + "}";
    }

    std::atomic<bool> run_{false};
    std::thread signalThread_;
    std::thread controlThread_;
};

// ---------------------------------------------------------------------------
// The C table
// ---------------------------------------------------------------------------

Session* S(FoxSession* s) { return static_cast<Session*>(s); }

// NOTHING THROWS ACROSS THE C BOUNDARY: every entry point runs its body here,
// which turns any exception (bad_alloc above all) into FOXAPI_FAILED.
template <class F>
int32_t guarded(F&& f) noexcept {
    try {
        return f();
    } catch (...) {
        return FOXAPI_FAILED;
    }
}

// Every per-session entry point: NULL is BAD_ARGUMENT, a revoked login is
// DETACHED, and nothing throws.
template <class F>
int32_t onSession(FoxSession* s, F&& f) noexcept {
    if (s == nullptr) {
        return FOXAPI_BAD_ARGUMENT;
    }
    if (S(s)->detached.load(std::memory_order_relaxed)) {
        return FOXAPI_DETACHED;
    }
    return guarded([&] { return f(S(s)); });
}

int32_t FOXAPI_CALL apiCreate(const FoxEngineParams* p, FoxEngine** out) {
    if (out == nullptr) {
        return FOXAPI_BAD_ARGUMENT;
    }
    *out = nullptr;
    const char* opts = nullptr;
    if (p != nullptr) {
        if (p->structSize < FOXAPI_MIN_ENGINE_PARAMS) {
            return FOXAPI_BAD_ARGUMENT;
        }
        opts = p->options;
    }
    return guarded([&] {
        *out = new Engine(parseOptions(opts));
        return FOXAPI_OK;
    });
}

void FOXAPI_CALL apiDestroy(FoxEngine* e) {
    try {
        delete static_cast<Engine*>(e);
    } catch (...) {
    }
}

int32_t FOXAPI_CALL apiOpenSession(FoxEngine* e, const FoxSessionParams* p, FoxSession** out) {
    if (e == nullptr) return FOXAPI_BAD_ARGUMENT;
    return guarded([&] { return static_cast<Engine*>(e)->openSession(p, out); });
}

void FOXAPI_CALL apiCloseSession(FoxSession* s) {
    if (s == nullptr) return;
    try {
        S(s)->engine->closeSession(S(s));
    } catch (...) {
    }
}

int32_t FOXAPI_CALL apiHeartbeat(FoxSession* s) {
    return onSession(s, [&](Session* x) { return x->engine->heartbeat(x); });
}

int32_t FOXAPI_CALL apiReadState(FoxSession* s, FoxReceiverState* out) {
    return onSession(s, [&](Session* x) { return x->engine->readState(x, out); });
}

int32_t FOXAPI_CALL apiReadSpectrum(FoxSession* s, uint64_t since, FoxSpectrumInfo* info,
                                    float* bins, uint32_t cap) {
    return onSession(s, [&](Session* x) { return x->engine->readSpectrum(since, info, bins, cap); });
}

int32_t FOXAPI_CALL apiSubmit(FoxSession* s, const FoxCommand* c, uint32_t n, FoxSubmitResult* r) {
    return onSession(s, [&](Session* x) { return x->engine->submit(x, c, n, r); });
}

int32_t FOXAPI_CALL apiPollResults(FoxSession* s, FoxCommandResult* out, uint32_t cap) {
    return onSession(s, [&](Session* x) { return x->engine->pollResults(x, out, cap); });
}

int32_t FOXAPI_CALL apiSubscribe(FoxSession* s, uint64_t mask) {
    return onSession(s, [&](Session* x) { return x->engine->subscribe(x, mask); });
}

int32_t FOXAPI_CALL apiPollEvents(FoxSession* s, FoxEvent* out, uint32_t cap) {
    return onSession(s, [&](Session* x) { return x->engine->pollEvents(x, out, cap); });
}

int32_t FOXAPI_CALL apiReadAudio(FoxSession* s, uint64_t* cursor, float* out, uint32_t cap,
                                 FoxStreamInfo* info) {
    return onSession(s, [&](Session* x) { return x->engine->readAudio(x, cursor, out, cap, info); });
}

int32_t FOXAPI_CALL apiReadIq(FoxSession* s, uint64_t*, float*, uint32_t, FoxStreamInfo*) {
    return onSession(s, [&](Session*) { return FOXAPI_UNSUPPORTED; });
}

int32_t FOXAPI_CALL apiReadList(FoxSession* s, uint32_t list, uint32_t index, FoxListItem* out) {
    return onSession(s, [&](Session* x) { return x->engine->readList(list, index, out); });
}

int32_t FOXAPI_CALL apiGetSetting(FoxSession* s, const char* key, char* buf, size_t cap) {
    return onSession(s, [&](Session* x) { return x->engine->getSetting(key, buf, cap); });
}

int32_t FOXAPI_CALL apiReadImage(FoxSession* s, uint64_t, FoxImageInfo*, uint8_t*, size_t) {
    return onSession(s, [&](Session*) { return FOXAPI_UNSUPPORTED; });
}

int32_t FOXAPI_CALL apiLogin(FoxEngine* e, const char* user, const char* password, char* token,
                             size_t cap) {
    if (e == nullptr) return FOXAPI_BAD_ARGUMENT;
    return guarded([&] { return static_cast<Engine*>(e)->login(user, password, token, cap); });
}

int32_t FOXAPI_CALL apiLogout(FoxEngine* e, const char* token) {
    if (e == nullptr) return FOXAPI_BAD_ARGUMENT;
    return guarded([&] { return static_cast<Engine*>(e)->logout(token); });
}

const FoxEngineApi kApi = {
    sizeof(FoxEngineApi),
    FOXAPI_VERSION_MAJOR,
    FOXAPI_VERSION_MINOR,
    0,
    FOXAPI_CAP_RECEIVER | FOXAPI_CAP_SPECTRUM | FOXAPI_CAP_AUDIO_LEVELS |
        FOXAPI_CAP_AUDIO_STREAM | FOXAPI_CAP_SOURCES | FOXAPI_CAP_GAINS |
        FOXAPI_CAP_AUDIO_DSP | FOXAPI_CAP_TRANSMIT | FOXAPI_CAP_BOOKMARKS |
        FOXAPI_CAP_SETTINGS | FOXAPI_CAP_EVENTS,
    "FoxSDR mock engine",
    "0.2.4",
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
};

}  // namespace

const FoxEngineApi* engineApi(uint32_t apiMajor, uint32_t apiMinor) {
    if (apiMajor != FOXAPI_VERSION_MAJOR) {
        return nullptr;
    }
    // During 0.x the draft may change what a member MEANS between minors
    // (0.2 changed what submit answers), so only the same minor is served.
    if (FOXAPI_VERSION_MAJOR == 0u && apiMinor != FOXAPI_VERSION_MINOR) {
        return nullptr;
    }
    return &kApi;
}

}  // namespace foxsdr::mock
