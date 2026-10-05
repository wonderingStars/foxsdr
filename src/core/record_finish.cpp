// record_finish.cpp - see record_finish.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/record_finish.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace cascade::core {

namespace {

// Leaked on purpose: see the header.
struct State {
    std::mutex m;
    std::condition_variable cv;
    std::size_t out = 0;
};

State& state() {
    static State* s = new State();
    return *s;
}

}  // namespace

std::shared_ptr<RecordFinisher::Ticket> RecordFinisher::submit(Recorder::FinishRequest&& req) {
    auto ticket = std::make_shared<Ticket>();
    if (!req.file) {
        ticket->done.store(true);  // nothing to finish
        return ticket;
    }
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.m);
        ++st.out;
    }
    // A thread of its own per finish, detached: the request is moved into it, so
    // nothing it touches outlives it, and a quit that cannot wait just leaves it.
    std::thread([req = std::move(req), ticket]() mutable {
        bool ok = false;
        try {
            ok = Recorder::finishFile(std::move(req));
        } catch (...) {
            ok = false;
        }
        ticket->ok.store(ok);
        ticket->done.store(true);
        State& s = state();
        {
            std::lock_guard<std::mutex> lock(s.m);
            --s.out;
        }
        s.cv.notify_all();
    }).detach();
    return ticket;
}

bool RecordFinisher::drain(std::chrono::steady_clock::time_point deadline) {
    State& st = state();
    std::unique_lock<std::mutex> lock(st.m);
    return st.cv.wait_until(lock, deadline, [&st] { return st.out == 0; });
}

void RecordFinisher::externalBegin() {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    ++st.out;
}

void RecordFinisher::externalEnd() {
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.m);
        if (st.out > 0) { --st.out; }
    }
    st.cv.notify_all();
}

std::size_t RecordFinisher::inFlight() {
    State& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    return st.out;
}

}  // namespace cascade::core
