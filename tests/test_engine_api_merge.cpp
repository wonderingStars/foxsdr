// test_engine_api_merge.cpp - the merge of the safer commands, checked
// exhaustively against the REAL engine: the port of third_party/foxsdr_api/
// reference/tests/test_merge_exhaustive.cpp, through the C table only, with
// the engine's own test seams (test.gather, test.keyState - the reference's
// mock.*). The reference's comments are kept; differences are marked PORT.
//
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_api_serial.hpp"

using testing::cmd;
using testing::kAllGrants;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Op {
    uint32_t op;
    long long value;
    const char* name;
};
const Op kOps[5] = {{FOXAPI_OP_TX_LATCH, 0, "LATCH0"}, {FOXAPI_OP_TX_LATCH, 1, "LATCH1"},
                    {FOXAPI_OP_RUN, 0, "RUN0"},        {FOXAPI_OP_TX_CLOSE, 0, "TX_CLOSE"},
                    {FOXAPI_OP_TX_PTT, 0, "PTT0"}};
const char kNames[3] = {'A', 'B', 'C'};

enum Start { kLatchedByA, kMarkedBExempt, kIdle, kStartCount };
const char* const kStartNames[kStartCount] = {"A holds the latch", "marked, B exempt, no latch", "nothing yet"};

struct Step {
    int session;
    int op;
};

// PORT: ONE ENGINE PER WORKER, NOT PER RUN. The reference builds a fresh
// mock (microseconds) for each of the ~141,000 runs; a real engine is a
// pipeline, a plugin scan and an audio-device probe, so a worker keeps one
// engine and its admin session, and every run opens fresh sessions A, B, C
// and starts from the same clean state: the transmitter closed, the receiver
// running, and the key rules reset (test.resetKey - no key, latch, mark or
// consent). The key state is compared with the sessions' ids replaced by
// their letters (test.sessionId), since each run's sessions have new ids.
class Engine1 {
public:
    Engine1() : e_(api(), "testHooks=1;token=t"), admin_(api(), e_.raw(), FOXAPI_SESSION_LOCAL, kAllGrants) {}
    FoxEngine* raw() const { return e_.raw(); }
    testing::Session& admin() { return admin_; }

private:
    engine_api_test::SerialEngine e_;
    testing::Session admin_;
};

class Rig {
public:
    explicit Rig(Engine1& eng) : eng_(eng) {
        for (int i = 0; i < 3; ++i) {
            s_[i] = std::make_unique<testing::Session>(api(), eng_.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
            ids_[i] = sessionId(*s_[i]);
        }
        adminId_ = sessionId(admin());
        bool got = false;
        runOne(admin(), cmd(FOXAPI_OP_TX_CLOSE), &got);
        runOne(admin(), cmd(FOXAPI_OP_RUN, 0, 1), &got);
        runOne(admin(), cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.resetKey=1"), &got);
    }

    testing::Session& admin() { return eng_.admin(); }
    testing::Session& at(int i) { return *s_[static_cast<std::size_t>(i)]; }
    void beatAll() {
        admin().beat();
        for (auto& s : s_) s->beat();
    }

    // Submits one command and waits (spinning, not sleeping: Windows sleeps
    // in 15 ms steps) for its result. Returns the submit status.
    int32_t runOne(testing::Session& s, const FoxCommand& c, bool* ok = nullptr) {
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        api()->submit(s.raw(), &c, 1, &sr);
        const bool got = sr.status == FOXAPI_OK ? waitTicket(s, sr.ticket) : true;
        if (ok != nullptr) *ok = got;
        return sr.status;
    }

    bool waitTicket(testing::Session& s, uint64_t ticket) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < end) {
            FoxCommandResult r[16];
            for (auto& x : r) x.structSize = sizeof(x);
            const int32_t n = api()->poll_results(s.raw(), r, 16);
            for (int32_t i = 0; i < n; ++i) {
                if (r[i].ticket == ticket) return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    // The key state, with every session id this run knows replaced by its
    // letter (S for the admin session): runs differ only in their ids.
    std::string keyState() {
        char buf[1024] = {};
        api()->get_setting(admin().raw(), "test.keyState", buf, sizeof(buf));
        std::string out;
        const std::string in = buf;
        for (std::size_t i = 0; i < in.size();) {
            if (in[i] >= '0' && in[i] <= '9' && (i == 0 || !std::isalnum(static_cast<unsigned char>(in[i - 1])))) {
                std::size_t j = i;
                while (j < in.size() && in[j] >= '0' && in[j] <= '9') ++j;
                const unsigned long long v = std::stoull(in.substr(i, j - i));
                std::string name = in.substr(i, j - i);
                for (int k = 0; k < 3; ++k) {
                    if (v == ids_[k] && v > 1) name = std::string(1, kNames[k]);
                }
                if (v == adminId_ && v > 1) name = "S";
                out += name;
                i = j;
            } else {
                out += in[i++];
            }
        }
        return out;
    }

private:
    static unsigned long long sessionId(testing::Session& s) {
        char buf[32] = {};
        api()->get_setting(s.raw(), "test.sessionId", buf, sizeof(buf));
        return std::strtoull(buf, nullptr, 10);
    }
    Engine1& eng_;
    std::unique_ptr<testing::Session> s_[3];
    unsigned long long ids_[3] = {};
    unsigned long long adminId_ = 0;
};

bool setUp(Rig& r, Start start) {
    r.beatAll();
    bool ok = true;
    bool got = false;
    ok = ok && r.runOne(r.admin(), cmd(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy"), &got) == FOXAPI_OK && got;
    if (start == kLatchedByA || start == kMarkedBExempt) {
        ok = ok && r.runOne(r.at(0), cmd(FOXAPI_OP_TX_LATCH, 0, 1), &got) == FOXAPI_OK && got;
    }
    if (start == kMarkedBExempt) {
        ok = ok && r.runOne(r.admin(), cmd(FOXAPI_OP_RUN, 0, 0), &got) == FOXAPI_OK && got;
        ok = ok && r.runOne(r.admin(), cmd(FOXAPI_OP_RUN, 0, 1), &got) == FOXAPI_OK && got;
        ok = ok && r.runOne(r.at(1), cmd(FOXAPI_OP_TX_LATCH, 0, 0), &got) == FOXAPI_OK && got;
    }
    r.beatAll();
    return ok;
}

struct Outcome {
    std::string state;
    int merged = 0;      // submits answered NO_CHANGE (merged run only)
    bool clean = true;   // every expected result arrived
    long long spanMs = 0;  // wall time of the whole run, from before its sessions opened to after the state was read
};

long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

// THE RULES THAT JUDGE WALL TIME. Two key rules compare the engine's clock with a fixed 1,000 ms: rule 4 (no latch
// closes within FOXAPI_LATCH_REARM_MS of the last one ending) and rule 7 (a key holder that has not beaten for
// FOXAPI_KEEPALIVE_MS has stopped answering). A case whose starting state ended a latch, followed by a LATCH 1,
// is only deterministic if the run takes less than that window: inside it the LATCH 1 is refused, in the merged run
// and the one-a-pass run alike; a run that stalls past it (24 workers and the control threads on a loaded host)
// has the LATCH 1 granted, and the other run, which did not stall, does not. That is a difference of timing, not of
// merging. A run's span contains the engine's whole interval from the latch's end to the last command's
// application, so a span under the window proves the rules never saw it pass.
constexpr long long kRuleWindowMs = (FOXAPI_LATCH_REARM_MS < FOXAPI_KEEPALIVE_MS ? FOXAPI_LATCH_REARM_MS
                                                                                 : FOXAPI_KEEPALIVE_MS) - 100;
constexpr int kMaxAttempts = 5;

// All of `seq` in one control pass: repeats of a session's safer command merge.
Outcome runMerged(Engine1& eng, Start start, const std::vector<Step>& seq) {
    const auto t0 = std::chrono::steady_clock::now();
    Rig r(eng);
    Outcome o;
    o.clean = setUp(r, start);
    bool got = false;
    const std::string g = "test.gather=" + std::to_string(seq.size());
    o.clean = o.clean && r.runOne(r.admin(), cmd(FOXAPI_OP_SETTING_SET, 0, 0, g.c_str()), &got) == FOXAPI_OK && got;
    std::vector<std::pair<int, uint64_t>> tickets;
    for (const Step& st : seq) {
        const Op& op = kOps[st.op];
        FoxCommand c = cmd(op.op, 0, op.value);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        api()->submit(r.at(st.session).raw(), &c, 1, &sr);
        if (sr.status == FOXAPI_OK) {
            tickets.push_back({st.session, sr.ticket});
        } else if (sr.status == FOXAPI_NO_CHANGE) {
            ++o.merged;
        } else {
            o.clean = false;
        }
    }
    // Every NEW ticket's result, whichever session - read all of each
    // session's results, since one poll can carry several tickets.
    std::vector<uint64_t> seen;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto allSeen = [&] {
        for (const auto& t : tickets) {
            if (std::find(seen.begin(), seen.end(), t.second * 4 + static_cast<uint64_t>(t.first)) == seen.end()) {
                return false;
            }
        }
        return true;
    };
    while (!allSeen() && std::chrono::steady_clock::now() < end) {
        for (int s = 0; s < 3; ++s) {
            FoxCommandResult rr[16];
            for (auto& x : rr) x.structSize = sizeof(x);
            const int32_t n = api()->poll_results(r.at(s).raw(), rr, 16);
            for (int32_t i = 0; i < n; ++i) {
                seen.push_back(rr[i].ticket * 4 + static_cast<uint64_t>(s));
            }
        }
        std::this_thread::yield();
    }
    o.clean = o.clean && allSeen();
    o.state = r.keyState();
    o.spanMs = msSince(t0);
    return o;
}

// The same, one command per pass, each result read before the next is sent:
// nothing merges.
Outcome runSeparately(Engine1& eng, Start start, const std::vector<Step>& seq) {
    const auto t0 = std::chrono::steady_clock::now();
    Rig r(eng);
    Outcome o;
    o.clean = setUp(r, start);
    for (const Step& st : seq) {
        const Op& op = kOps[st.op];
        bool got = false;
        const int32_t status = r.runOne(r.at(st.session), cmd(op.op, 0, op.value), &got);
        o.clean = o.clean && status == FOXAPI_OK && got;
    }
    o.state = r.keyState();
    o.spanMs = msSince(t0);
    return o;
}

// Sessions the starting state treats alike appear in a fixed order of first
// use, so each case up to renaming is run once.
bool canonical(Start start, const std::vector<Step>& seq) {
    std::vector<int> firstSeen;
    for (const Step& st : seq) {
        if (std::find(firstSeen.begin(), firstSeen.end(), st.session) == firstSeen.end()) {
            firstSeen.push_back(st.session);
        }
    }
    auto before = [&](int x, int y) {  // x is used, and first, if y is used at all
        const auto ix = std::find(firstSeen.begin(), firstSeen.end(), x);
        const auto iy = std::find(firstSeen.begin(), firstSeen.end(), y);
        return iy == firstSeen.end() || (ix != firstSeen.end() && ix < iy);
    };
    switch (start) {
    case kLatchedByA: return before(1, 2);
    case kMarkedBExempt: return before(0, 2);
    default: return before(0, 1) && before(1, 2);
    }
}

std::string describe(const std::vector<Step>& seq) {
    std::string s;
    for (const Step& st : seq) {
        s += std::string(1, kNames[st.session]) + ":" + kOps[st.op].name + " ";
    }
    return s;
}

}  // namespace

int main() {
    engine_api_test::hermetic("merge");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    struct Case {
        Start start;
        std::vector<Step> seq;
    };
    // PORT: HOW MUCH OF THE SWEEP. The reference runs every case of 1-4
    // commands (64,450) and 6,000 random ones of 5-8 in a few seconds against
    // the mock; against the real engine that is tens of minutes (each latch
    // starts the real TX thread and each release plays its ramp), so the
    // suite runs every case of 1-3 commands and 1,000 random ones of 5-8, and
    // FOXSDR_MERGE_FULL=1 runs the reference's whole sweep (docs/
    // engine-stage4.md records a full run).
    const bool full = std::getenv("FOXSDR_MERGE_FULL") != nullptr && std::string(std::getenv("FOXSDR_MERGE_FULL")) == "1";
    const int maxLen = full ? 4 : 3;
    const int randomCases = full ? 6000 : 1000;
    std::vector<Case> cases;
    for (int s = 0; s < kStartCount; ++s) {
        for (int len = 1; len <= maxLen; ++len) {
            int total = 1;
            for (int i = 0; i < len; ++i) total *= 15;
            for (int code = 0; code < total; ++code) {
                std::vector<Step> seq;
                int c = code;
                for (int i = 0; i < len; ++i) {
                    seq.push_back({(c % 15) / 5, c % 5});
                    c /= 15;
                }
                if (canonical(static_cast<Start>(s), seq)) {
                    cases.push_back({static_cast<Start>(s), seq});
                }
            }
        }
    }
    const std::size_t exhaustive = cases.size();
    // Beyond 4: a fixed-seed sample of 5 to 8 commands, where a session can
    // repeat a kind three or more times with other commands between (the
    // shape that lost a middle send in round 5's design).
    std::mt19937 rng(20260925u);
    for (int i = 0; i < randomCases; ++i) {
        const int len = 5 + static_cast<int>(rng() % 4);
        std::vector<Step> seq;
        for (int k = 0; k < len; ++k) {
            seq.push_back({static_cast<int>(rng() % 3), static_cast<int>(rng() % 5)});
        }
        cases.push_back({static_cast<Start>(rng() % kStartCount), seq});
    }
    std::atomic<std::size_t> next{0};
    std::atomic<int> mergedCases{0}, unclean{0}, rerun{0};
    std::mutex failMutex;
    std::vector<std::string> failures;
    const unsigned workers = std::max(2u, std::min(24u, std::thread::hardware_concurrency()));
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            // PORT: THE ENGINE IS THE WORKER'S, ON ITS OWN STACK. Never a
            // thread_local: Windows runs thread_local destructors inside
            // LdrShutdownThread with the process loader lock held, and
            // ~Engine1 joins the engine's control thread (which joins the
            // transmitter's TX thread), neither of which can finish exiting
            // without that lock - the whole pool deadlocks at worker exit.
            // Never join a thread from a thread_local destructor.
            auto engine = std::make_unique<Engine1>();
            for (std::size_t i = next++; i < cases.size(); i = next++) {
                const Case& k = cases[i];
                // A difference is excused only when one of the two runs outlasted the window the rules judge time
                // by (kRuleWindowMs): the case is then run again, up to kMaxAttempts times. A difference between
                // two runs that both finished inside the window is never excused, and neither is one that
                // survives every attempt.
                Outcome m, u;
                for (int attempt = 1;; ++attempt) {
                    m = runMerged(*engine, k.start, k.seq);
                    u = runSeparately(*engine, k.start, k.seq);
                    const bool differs = m.state != u.state || m.state.empty();
                    const bool slow = std::max(m.spanMs, u.spanMs) >= kRuleWindowMs;
                    if (!differs || !slow || attempt >= kMaxAttempts) break;
                    ++rerun;
                    std::lock_guard<std::mutex> lk(failMutex);
                    std::printf("RERUN (a run outlasted the %lld ms the rules judge time by: merged %lld ms, one a pass %lld ms): %s | %s\n",
                                kRuleWindowMs, m.spanMs, u.spanMs, kStartNames[k.start], describe(k.seq).c_str());
                }
                if (m.merged > 0) ++mergedCases;
                if (!m.clean || !u.clean) ++unclean;
                if (m.state != u.state || m.state.empty()) {
                    std::lock_guard<std::mutex> lk(failMutex);
                    failures.push_back(std::string(kStartNames[k.start]) + " | " + describe(k.seq) +
                                       "\n    merged (" + std::to_string(m.spanMs) + " ms):     " + m.state + "\n    one a pass (" +
                                       std::to_string(u.spanMs) + " ms): " + u.state);
                }
            }
            engine.reset();  // the engine is gone before the thread function returns
        });
    }
    for (auto& t : pool) t.join();
    const long long secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu cases (%zu exhaustive: 3 starting states, 1-%d commands from A/B/C over LATCH0 LATCH1 RUN0 "
                "TX_CLOSE PTT0, up to renaming; %zu random of 5-8 commands), %d with at least one merged send, on %u "
                "threads in %lld s: %zu differ, %d with a missing result or refusal at setup, %d rerun for a stall\n",
                cases.size(), exhaustive, maxLen, cases.size() - exhaustive, mergedCases.load(), workers, secs,
                failures.size(), unclean.load(), rerun.load());
    for (std::size_t i = 0; i < failures.size() && i < 12; ++i) {
        std::printf("DIFFERS: %s\n", failures[i].c_str());
    }
    CHECK(exhaustive > (full ? 10000u : 1000u));
    CHECK(mergedCases.load() > (full ? 1000 : 200));  // the sweep really merged
    CHECK_EQ(failures.size(), static_cast<std::size_t>(0));
    CHECK_EQ(unclean.load(), 0);
    return check::finish("test_engine_api_merge");
}
