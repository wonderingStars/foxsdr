// Tests for the browser's named-profile list and switch gate (net/web_profiles.hpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "net/web_profiles.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

void testListAndActive() {
    WebProfileBook b;
    CHECK(b.list().empty());                                       // empty is a valid state: no profiles
    CHECK(b.setNames({"HF Night", "Airband", "FM Local"}, "Airband") == ProfileResult::Ok);
    const auto l = b.list();
    CHECK(l.size() == 3);
    CHECK(l.size() == 3 && l[0].name == "HF Night" && !l[0].active && l[1].name == "Airband" && l[1].active && !l[2].active);
    // an active name that is not in the list means none is active
    CHECK(b.setNames({"A", "B"}, "Z") == ProfileResult::Ok);
    for (const auto& e : b.list()) { CHECK(!e.active); }
    CHECK(b.setNames({}, "") == ProfileResult::Ok);
    CHECK(b.list().empty());
}

void testListValidation() {
    WebProfileBook b;
    CHECK(b.setNames({"Keep"}, "Keep") == ProfileResult::Ok);
    CHECK(b.setNames({"ok", ""}, "") == ProfileResult::BadList);
    CHECK(b.setNames({"ok", "   "}, "") == ProfileResult::BadList);
    CHECK(b.setNames({"ok", "tab\there"}, "") == ProfileResult::BadList);
    CHECK(b.setNames({"ok", std::string(kMaxProfileName + 1, 'x')}, "") == ProfileResult::BadList);
    CHECK(b.setNames({"ok", std::string(kMaxProfileName, 'x')}, "") == ProfileResult::Ok);       // the boundary
    CHECK(b.setNames({"Dup", "dup"}, "") == ProfileResult::BadList);                               // case-only duplicate
    CHECK(b.setNames({"Dup", "Dup"}, "") == ProfileResult::BadList);
    CHECK(b.setNames({"bad\xC0\xAF"}, "") == ProfileResult::BadList);
    std::vector<std::string> many;
    for (std::size_t i = 0; i < kMaxProfiles; ++i) { many.push_back("p" + std::to_string(i)); }
    CHECK(b.setNames(many, "") == ProfileResult::Ok);                                              // exactly the cap
    many.push_back("one-too-many");
    CHECK(b.setNames(many, "") == ProfileResult::BadList);
    // a refused list left the previous one in place
    CHECK(b.list().size() == kMaxProfiles);
}

void testSwitchHappyPath() {
    WebProfileBook b;
    b.setNames({"A", "B", "C"}, "A");
    std::string name;
    CHECK(b.requestSwitch("B", 1000, name) == ProfileResult::Ok);
    CHECK(name == "B");
    CHECK(b.switching(1000));
    // not yet active until the host answers
    for (const auto& e : b.list()) { CHECK(e.active == (e.name == "A")); }
    CHECK(b.completeSwitch(true, 1100) == ProfileResult::Ok);
    CHECK(!b.switching(1100));
    for (const auto& e : b.list()) { CHECK(e.active == (e.name == "B")); }
    // nothing pending now
    CHECK(b.completeSwitch(true, 1200) == ProfileResult::NoSwitchInFlight);
}

void testSwitchRefusals() {
    WebProfileBook b;
    b.setNames({"Alpha", "Beta"}, "Alpha");
    std::string name = "stale";
    CHECK(b.requestSwitch("Gamma", 0, name) == ProfileResult::Unknown);
    CHECK(name.empty());                                           // nothing is handed on for a refusal
    // only an exact match counts: not case-folded, not trimmed, not a prefix, not with a control character
    CHECK(b.requestSwitch("beta", 0, name) == ProfileResult::Unknown);
    CHECK(b.requestSwitch(" Beta", 0, name) == ProfileResult::Unknown);
    CHECK(b.requestSwitch("Bet", 0, name) == ProfileResult::Unknown);
    CHECK(b.requestSwitch(std::string("Beta\0x", 6), 0, name) == ProfileResult::Unknown);
    CHECK(b.requestSwitch("", 0, name) == ProfileResult::Unknown);
    CHECK(b.requestSwitch("Alpha", 0, name) == ProfileResult::AlreadyActive);
    CHECK(!b.switching(0));                                        // none of those started a switch
    // an empty book refuses everything
    WebProfileBook none;
    CHECK(none.requestSwitch("Alpha", 0, name) == ProfileResult::Unknown);
}

void testBusyRateLimitAndTimeout() {
    WebProfileBook b;
    b.setNames({"A", "B", "C"}, "A");
    std::string name;
    CHECK(b.requestSwitch("B", 10000, name) == ProfileResult::Ok);
    CHECK(b.requestSwitch("C", 10001, name) == ProfileResult::Busy);                    // one at a time
    CHECK(name.empty());
    CHECK(b.completeSwitch(false, 10050) == ProfileResult::Ok);                         // the host refused
    for (const auto& e : b.list()) { CHECK(e.active == (e.name == "A")); }              // active unchanged by a failed switch
    // rate limit: kMinSwitchGapMs after the START of the previous one
    CHECK(b.requestSwitch("C", 10000 + kMinSwitchGapMs - 1, name) == ProfileResult::RateLimited);
    CHECK(b.requestSwitch("C", 10000 + kMinSwitchGapMs, name) == ProfileResult::Ok);     // the boundary
    CHECK(b.completeSwitch(true, 12500) == ProfileResult::Ok);
    // a switch that is never answered stops blocking after the timeout
    CHECK(b.requestSwitch("A", 20000, name) == ProfileResult::Ok);
    CHECK(b.requestSwitch("B", 20000 + kSwitchTimeoutMs - 1, name) == ProfileResult::Busy);
    CHECK(b.switching(20000 + kSwitchTimeoutMs - 1));
    CHECK(!b.switching(20000 + kSwitchTimeoutMs));
    CHECK(b.requestSwitch("B", 20000 + kSwitchTimeoutMs, name) == ProfileResult::Ok);
    // a refused request (unknown name) does not consume the rate limit window
    WebProfileBook c;
    c.setNames({"A", "B"}, "A");
    CHECK(c.requestSwitch("zzz", 5000, name) == ProfileResult::Unknown);
    CHECK(c.requestSwitch("B", 5000, name) == ProfileResult::Ok);
}

void testListReplacedMidSwitch() {
    WebProfileBook b;
    b.setNames({"A", "B"}, "A");
    std::string name;
    CHECK(b.requestSwitch("B", 100, name) == ProfileResult::Ok);
    // the host renamed things: B is gone while the switch is in flight
    CHECK(b.setNames({"A", "C"}, "A") == ProfileResult::Ok);
    CHECK(!b.switching(150));
    CHECK(b.completeSwitch(true, 160) == ProfileResult::NoSwitchInFlight);
    for (const auto& e : b.list()) { CHECK(e.active == (e.name == "A")); }
    // a switch to a name that survived the replacement stays in flight and completes
    WebProfileBook d;
    d.setNames({"A", "B"}, "A");
    CHECK(d.requestSwitch("B", 100, name) == ProfileResult::Ok);
    CHECK(d.setNames({"B", "A", "C"}, "A") == ProfileResult::Ok);
    CHECK(d.switching(150));
    CHECK(d.completeSwitch(true, 160) == ProfileResult::Ok);
    for (const auto& e : d.list()) { CHECK(e.active == (e.name == "B")); }
}

void testConcurrency() {
    WebProfileBook b;
    b.setNames({"A", "B", "C", "D"}, "A");
    std::atomic<std::int64_t> clock{0};
    std::atomic<int> started{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> overlap{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&, t] {
            const char* names[] = {"A", "B", "C", "D"};
            for (int i = 0; i < 500; ++i) {
                std::string n;
                const std::int64_t now = clock.fetch_add(1) + 1;
                if (b.requestSwitch(names[(t + i) % 4], now, n) == ProfileResult::Ok) {
                    if (concurrent.fetch_add(1) != 0) { ++overlap; }   // two switches in flight at once would be a bug
                    ++started;
                    for (int spin = 0; spin < 300; ++spin) { std::this_thread::yield(); }   // widen the window a broken gate would be seen in
                    concurrent.fetch_sub(1);
                    b.completeSwitch(true, now);
                    clock.fetch_add(kMinSwitchGapMs + 1);                       // let the rate limit window pass so the next start can be allowed
                }
            }
        });
    }
    for (auto& th : ts) { th.join(); }
    CHECK(started.load() > 0);
    CHECK(overlap.load() == 0);
    int active = 0;
    for (const auto& e : b.list()) { active += e.active ? 1 : 0; }
    CHECK(active == 1);
}

}  // namespace

int main() {
    testListAndActive();
    testListValidation();
    testSwitchHappyPath();
    testSwitchRefusals();
    testBusyRateLimitAndTimeout();
    testListReplacedMidSwitch();
    testConcurrency();
    return testSummary("test_web_profiles");
}
