// plugin_start_worker.cpp - see plugin_start_worker.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_start_worker.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/leak_on_purpose.hpp"
#include "core/stall_context.hpp"

namespace cascade::core {

namespace {

std::int64_t realMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// LEAKED ON PURPOSE, as the thread registry's state is: a worker parked inside a plugin outlives main().
struct State {
    std::mutex mu;
    std::condition_variable cv;        // a job was queued, or a generation was abandoned
    std::condition_variable drainedCv; // a job finished
    std::deque<StartWorker::Job> queue;
    bool running = false;              // a thread is started
    std::uint64_t activeGen = 0;       // the generation of the thread that owns the queue
    bool busy = false;
    StartWorker::Job cur;              // the job in flight (run emptied)
    std::int64_t curStartMs = 0;
    std::size_t abandoned = 0;
};

State& st() {
    static State* const s = cascade::core::leakOnPurpose(new State());
    return *s;
}

void threadMain(std::uint64_t gen);

// (mutex held) starts the thread for generation `gen`.
void spawn(std::uint64_t gen) {
    std::thread(threadMain, gen).detach();
}

void threadMain(std::uint64_t gen) {
    stall::registerThreadRole(stall::Role::Other);   // (a report calls this thread `other`: it is neither the window, the pass, the DSP nor a source)
    State& s = st();
    for (;;) {
        StartWorker::Job job;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.cv.wait(lk, [&] { return gen != s.activeGen || !s.queue.empty(); });
            if (gen != s.activeGen) { return; }   // abandoned while idle: cannot happen (only a busy thread is abandoned), but ends cleanly
            job = std::move(s.queue.front());
            s.queue.pop_front();
            s.busy = true;
            s.cur.owner = job.owner;
            s.cur.key = job.key;
            s.cur.isCreate = job.isCreate;
            s.curStartMs = StartWorker::nowMs();
        }
        try {
            if (job.run) { job.run(); }
        } catch (...) {
            // (a throw out of a job - not out of a plugin, whose calls are guarded - is the job's own; the worker goes on)
        }
        {
            std::lock_guard<std::mutex> lk(s.mu);
            if (gen != s.activeGen) {
                // ABANDONED WHILE IN THE JOB, and the call has now returned: the replacement owns the queue and the state; this thread ends.
                if (s.abandoned > 0) { --s.abandoned; }
                s.drainedCv.notify_all();
                return;
            }
            s.busy = false;
            s.cur = StartWorker::Job{};
        }
        s.drainedCv.notify_all();
    }
}

}  // namespace

StartWorker& StartWorker::instance() {
    static StartWorker* const w = cascade::core::leakOnPurpose(new StartWorker());
    return *w;
}

std::int64_t StartWorker::nowMs() { return realMs(); }

void StartWorker::post(Job job) {
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.queue.push_back(std::move(job));
        if (!s.running) {
            s.running = true;
            spawn(s.activeGen);
        }
    }
    s.cv.notify_all();
}

StartWorker::Current StartWorker::current() const {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    Current c;
    c.busy = s.busy;
    if (s.busy) {
        c.owner = s.cur.owner;
        c.key = s.cur.key;
        c.isCreate = s.cur.isCreate;
        c.sinceMs = nowMs() - s.curStartMs;
    }
    return c;
}

std::size_t StartWorker::pendingFor(std::uint64_t owner) const {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    std::size_t n = (s.busy && s.cur.owner == owner) ? 1u : 0u;
    for (const Job& j : s.queue) { if (j.owner == owner) { ++n; } }
    return n;
}

bool StartWorker::waitDrained(std::uint64_t owner, std::chrono::milliseconds bound) {
    State& s = st();
    std::unique_lock<std::mutex> lk(s.mu);
    const auto idle = [&] {
        if (s.busy && s.cur.owner == owner) { return false; }
        for (const Job& j : s.queue) { if (j.owner == owner) { return false; } }
        return true;
    };
    return s.drainedCv.wait_for(lk, bound, idle);
}

bool StartWorker::abandonCurrent(const std::string& expectKey) {
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.busy) { return false; }
        if (!expectKey.empty() && s.cur.key != expectKey) { return false; }
        if (s.abandoned >= kMaxAbandoned) { return false; }
        ++s.abandoned;
        ++s.activeGen;       // the thread inside the job now belongs to a past generation
        s.busy = false;
        s.cur = Job{};
        spawn(s.activeGen);  // the replacement takes the queue
    }
    s.cv.notify_all();
    s.drainedCv.notify_all();
    return true;
}

std::size_t StartWorker::abandoned() const {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.abandoned;
}

void StartWorker::resetForTest() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.queue.clear();
    s.abandoned = 0;
}

}  // namespace cascade::core
