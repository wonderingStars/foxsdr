// test_merge_exhaustive.cpp - "sent once or N times, merged or not, the same
// state", checked for EVERY small case rather than argued.
//
// Round-6 review (L6): a merged safer command was applied in the place of its
// LAST send, so B LATCH 0, A LATCH 0 (A's own release), B LATCH 0 in one pass
// ended A's latch as A's own release - unmarked - where the same commands one
// pass each ended it as B's release, marked. The engine now keeps every send
// in its own place and drops a merged send only when it cannot change
// anything (docs/API.md 7.2). This test holds it to that exhaustively:
//
//   three sessions A, B, C of the local operator; every sequence of 1 to 4
//   commands, each from any of them, drawn from LATCH 0, LATCH 1, RUN 0,
//   TX_CLOSE and PTT 0; from three starting states (A holds the latch; the
//   operator is marked with B exempt and no latch; nothing has happened);
//   run once with all of it in ONE control pass (mock.gather: repeats of a
//   session's safer command MERGE - submit answers NO_CHANGE) and once with
//   each command in its own pass and its result read first (nothing merges).
//   The engine's key state afterwards (mock.keyState: key holder, latch,
//   hold, running, transmitter, consent, why the key and the last latch
//   ended, every mark and its exempt sessions) must be identical.
//
// Sequences that differ only by renaming sessions the starting state treats
// alike are run once (B/C when A holds the latch, A/C when B is exempt, all
// three from nothing).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
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
#include "mock_engine/mock_engine.hpp"

using testing::cmd;
using testing::kAllGrants;

namespace {

const FoxEngineApi* api() { return foxsdr::mock::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

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

// One engine with an admin session (the seams) and A, B, C, opened in that
// order so both runs of a case number them alike.
class Rig {
public:
    Rig()
        : e_(api(), "bins=64;fps=240;controlHz=20;token=t"),
          admin_(api(), e_.raw(), FOXAPI_SESSION_LOCAL, kAllGrants) {
        for (auto& s : s_) {
            s = std::make_unique<testing::Session>(api(), e_.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        }
    }
    testing::Session& admin() { return admin_; }
    testing::Session& at(int i) { return *s_[static_cast<std::size_t>(i)]; }
    void beatAll() {
        admin_.beat();
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

    std::string keyState() {
        char buf[1024] = {};
        api()->get_setting(admin_.raw(), "mock.keyState", buf, sizeof(buf));
        return buf;
    }

private:
    testing::Engine e_;
    testing::Session admin_;
    std::unique_ptr<testing::Session> s_[3];
};

bool setUp(Rig& r, Start start) {
    r.beatAll();
    bool ok = true;
    bool got = false;
    ok = ok && r.runOne(r.admin(), cmd(FOXAPI_OP_TX_OPEN, 0, 0, "mock:tx:dummy"), &got) == FOXAPI_OK && got;
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
};

// All of `seq` in one control pass: repeats of a session's safer command merge.
Outcome runMerged(Start start, const std::vector<Step>& seq) {
    Rig r;
    Outcome o;
    o.clean = setUp(r, start);
    bool got = false;
    const std::string g = "mock.gather=" + std::to_string(seq.size());
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
    return o;
}

// The same, one command per pass, each result read before the next is sent:
// nothing merges.
Outcome runSeparately(Start start, const std::vector<Step>& seq) {
    Rig r;
    Outcome o;
    o.clean = setUp(r, start);
    for (const Step& st : seq) {
        const Op& op = kOps[st.op];
        bool got = false;
        const int32_t status = r.runOne(r.at(st.session), cmd(op.op, 0, op.value), &got);
        o.clean = o.clean && status == FOXAPI_OK && got;
    }
    o.state = r.keyState();
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    struct Case {
        Start start;
        std::vector<Step> seq;
    };
    std::vector<Case> cases;
    for (int s = 0; s < kStartCount; ++s) {
        for (int len = 1; len <= 4; ++len) {
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
    for (int i = 0; i < 6000; ++i) {
        const int len = 5 + static_cast<int>(rng() % 4);
        std::vector<Step> seq;
        for (int k = 0; k < len; ++k) {
            seq.push_back({static_cast<int>(rng() % 3), static_cast<int>(rng() % 5)});
        }
        cases.push_back({static_cast<Start>(rng() % kStartCount), seq});
    }
    std::atomic<std::size_t> next{0};
    std::atomic<int> mergedCases{0}, unclean{0};
    std::mutex failMutex;
    std::vector<std::string> failures;
    const unsigned workers = std::max(2u, std::min(24u, std::thread::hardware_concurrency()));
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < cases.size(); i = next++) {
                const Case& k = cases[i];
                const Outcome m = runMerged(k.start, k.seq);
                const Outcome u = runSeparately(k.start, k.seq);
                if (m.merged > 0) ++mergedCases;
                if (!m.clean || !u.clean) ++unclean;
                if (m.state != u.state || m.state.empty()) {
                    std::lock_guard<std::mutex> lk(failMutex);
                    failures.push_back(std::string(kStartNames[k.start]) + " | " + describe(k.seq) +
                                       "\n    merged:     " + m.state + "\n    one a pass: " + u.state);
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    const long long secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu cases (%zu exhaustive: 3 starting states, 1-4 commands from A/B/C over LATCH0 LATCH1 RUN0 "
                "TX_CLOSE PTT0, up to renaming; %zu random of 5-8 commands), %d with at least one merged send, on %u "
                "threads in %lld s: %zu differ, %d with a missing result or refusal at setup\n",
                cases.size(), exhaustive, cases.size() - exhaustive, mergedCases.load(), workers, secs,
                failures.size(), unclean.load());
    for (std::size_t i = 0; i < failures.size() && i < 12; ++i) {
        std::printf("DIFFERS: %s\n", failures[i].c_str());
    }
    CHECK(exhaustive > 10000);
    CHECK(mergedCases.load() > 1000);  // the sweep really merged
    CHECK_EQ(failures.size(), static_cast<std::size_t>(0));
    CHECK_EQ(unclean.load(), 0);
    return check::finish("test_merge_exhaustive");
}
