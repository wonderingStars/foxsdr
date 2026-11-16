// The device-independent event queue: order, bounded overflow that drops the OLDEST and counts, partial drain, producers on several threads, and a push that
// does not wait for a consumer that never comes.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>
#include <vector>

#include "input/input_event.hpp"
#include "test_check.hpp"

using namespace cascade::input;

static InputEvent ev(std::int32_t raw) {
    InputEvent e;
    e.device = 7;
    e.control = 1;
    e.kind = EventKind::Relative;
    e.raw = raw;
    return e;
}

int main() {
    // ORDER and size.
    {
        EventQueue q(8);
        for (int i = 1; i <= 5; ++i) { CHECK(q.push(ev(i))); }
        CHECK(q.size() == 5);
        std::vector<InputEvent> out;
        CHECK(q.drain(out) == 5);
        CHECK(out.size() == 5 && out[0].raw == 1 && out[4].raw == 5);
        CHECK(q.size() == 0 && q.dropped() == 0);
    }
    // OVERFLOW drops the oldest, counts it, keeps the newest `capacity` in order.
    {
        EventQueue q(4);
        for (int i = 1; i <= 10; ++i) { q.push(ev(i)); }
        CHECK(q.size() == 4);
        CHECK(q.dropped() == 6);
        std::vector<InputEvent> out;
        q.drain(out);
        CHECK(out.size() == 4 && out[0].raw == 7 && out[1].raw == 8 && out[2].raw == 9 && out[3].raw == 10);
        // Usable after a wrap-around.
        for (int i = 20; i < 23; ++i) { q.push(ev(i)); }
        out.clear();
        q.drain(out);
        CHECK(out.size() == 3 && out[0].raw == 20 && out[2].raw == 22);
    }
    // push() says false exactly when it dropped.
    {
        EventQueue q(2);
        CHECK(q.push(ev(1)));
        CHECK(q.push(ev(2)));
        CHECK(!q.push(ev(3)));
        CHECK(q.dropped() == 1);
    }
    // CAPACITY 1 and capacity 0 (clamped to 1).
    {
        EventQueue q(0);
        CHECK(q.capacity() == 1);
        q.push(ev(1));
        q.push(ev(2));
        std::vector<InputEvent> out;
        q.drain(out);
        CHECK(out.size() == 1 && out[0].raw == 2 && q.dropped() == 1);
    }
    // PARTIAL DRAIN keeps the rest in order.
    {
        EventQueue q(8);
        for (int i = 1; i <= 6; ++i) { q.push(ev(i)); }
        std::vector<InputEvent> out;
        CHECK(q.drain(out, 2) == 2);
        CHECK(out[0].raw == 1 && out[1].raw == 2 && q.size() == 4);
        out.clear();
        CHECK(q.drain(out, 100) == 4);
        CHECK(out[0].raw == 3 && out[3].raw == 6);
        CHECK(q.drain(out) == 0);
    }
    // DEVICE IDS: stable text -> stable id, different text -> different id.
    {
        CHECK(deviceIdFor("hid:0b33:0020") == deviceIdFor("hid:0b33:0020"));
        CHECK(deviceIdFor("hid:0b33:0020") != deviceIdFor("hid:077d:0410"));
        CHECK(deviceIdFor("") == 2166136261u);
    }
    // PRODUCERS ON FOUR THREADS and one consumer: nothing lost beyond what is counted, nothing duplicated, per-producer order kept.
    {
        EventQueue q(256);
        constexpr int kPer = 20000;
        std::vector<std::thread> th;
        for (int p = 0; p < 4; ++p) {
            th.emplace_back([&q, p] {
                for (int i = 0; i < kPer; ++i) {
                    InputEvent e = ev(i);
                    e.device = static_cast<std::uint32_t>(p);
                    q.push(e);
                }
            });
        }
        std::vector<InputEvent> got;
        bool done = false;
        std::thread cons([&] {
            while (!done) { q.drain(got, 64); std::this_thread::yield(); }
        });
        for (auto& t : th) { t.join(); }
        done = true;
        cons.join();
        q.drain(got);
        CHECK(got.size() + q.dropped() == 4u * kPer);
        int last[4] = {-1, -1, -1, -1};
        bool ordered = true;
        for (const auto& e : got) {
            if (e.raw <= last[e.device]) { ordered = false; }
            last[e.device] = e.raw;
        }
        CHECK(ordered);
    }
    // A PUSH NEVER WAITS FOR A CONSUMER: a million pushes into a queue nobody drains finish quickly and count the drops.
    {
        EventQueue q(16);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000000; ++i) { q.push(ev(i)); }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 5000);
        CHECK(q.dropped() == 1000000u - 16u);
    }
    return testSummary("test_input_event");
}
