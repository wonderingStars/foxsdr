// test_radio_reporter_review.cpp - the radio reporter's fixes from the review of fix/freezes (2026-11-23), finding 4.
//
//   A. THE "MAYBE QUEUED" BIT IS SET AFTER THE FILE IS WRITTEN. A failure report whose durable append is slow (the caller is inside the write) while the batch thread
//      finishes, clears the bit and finds the folder empty must still be sent in the same session: the bit is true when the write lands.
//   B. THE BATCH THREAD HOLDS NO LOCK ACROSS A LISTING. While the batch thread's listing of the reports folder is blocked for seconds (a slow disk), Reporter::event()
//      and Reporter::fileFailure() on the calling thread return at once, and a failure filed meanwhile is sent in the same session.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "test_check.hpp"

using namespace cascade::core::radioreport;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::int64_t kT0 = 1794830400;
const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
const char* const kReason = "the radio stopped answering while restoring the crystal trim: a control transfer failed";

struct Server {
    std::mutex mu;
    std::vector<std::string> bodies;
    Transport transport() {
        return [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return PostResult{true, 204, 0, ""};
        };
    }
    std::size_t count() { std::lock_guard<std::mutex> lk(mu); return bodies.size(); }
    std::size_t failures() {
        std::lock_guard<std::mutex> lk(mu);
        std::size_t n = 0;
        for (const std::string& b : bodies) { if (b.find("radio_fail") != std::string::npos) { ++n; } }
        return n;
    }
};

struct Rig {
    fs::path dir;
    std::atomic<std::int64_t> clock{kT0};
    std::shared_ptr<Server> server = std::make_shared<Server>();
    Reporter rep;
    explicit Rig(const char* tag) {
        dir = fs::temp_directory_path() / (std::string("rrr-") + tag + "-" + randomHex(8));
        fs::create_directories(dir);
        Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.nowEpoch = [this]() { return clock.load(); };
        o.transport = server->transport();
        o.liveSend = true;
        o.identity.installId = kInstallId;
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.channel = "installer";
        o.identity.updateChannel = "stable";
        o.identity.launches = 7;
        rep.configure(o);
    }
    ~Rig() {
        Reporter::setDiskHookForTest(nullptr);
        rep.shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

FailureInput failure(const char* args) {
    FailureInput f;
    f.driverKind = "airspyhf";
    f.args = args;
    f.reason = kReason;
    f.reasonClass = "control";
    f.trigger = Trigger::StartupRestore;
    return f;
}

const std::vector<std::string> kRing = {"12:01:07.120 info window: opened the main window", "12:01:07.331 warn source: the saved radio did not reopen"};

template <class Pred>
bool waitFor(Pred p, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

}  // namespace

int main() {
    // ------------------------------------------------------------------ A. THE BIT IS SET AFTER THE WRITE
    {
        std::printf("A: a failure whose write is slow, finished while the batch thread cleared the bit, is still sent in the session\n");
        Rig rig("bit");
        // The folder is empty. A batch thread is started (poll()) and held at its first listing until the failure's durable append has BEGUN; that write is then stalled by
        // the disk (the seam, on the calling thread only) while the batch thread lists the empty folder, clears the bit and lists again. The failure lands after all of that.
        const auto mainId = std::this_thread::get_id();
        std::atomic<bool> stalled{false};
        std::atomic<int> batchListings{0};
        Reporter::setDiskHookForTest([&] {
            if (std::this_thread::get_id() != mainId) {
                if (++batchListings == 1) {
                    const auto until = Clock::now() + std::chrono::seconds(5);
                    while (!stalled.load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
                }
                return;
            }
            if (stalled.exchange(true)) { return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(700));   // the batch thread's listings run in this time
        });
        rig.rep.poll();   // (the bit starts true: the folder may hold what an earlier session left)
        CHECK(waitFor([&] { return batchListings.load() >= 1; }, 3000));
        rig.rep.fileFailure(failure("serial=HF0001B3"), kRing);   // returns when the file is on disk (and, now, the bit is set)
        Reporter::setDiskHookForTest(nullptr);
        CHECK(stalled.load());
        CHECK(batchListings.load() >= 3);
        // The next pass: poll() must start a batch (the bit is true) and the failure must go.
        CHECK(waitFor([&] { rig.rep.poll(); return rig.server->count() == 1u; }, 5000));
        std::printf("   sent %zu of 1\n", rig.server->count());
    }

    // ------------------------------------------------------------------ B. NO LOCK ACROSS THE BATCH THREAD'S LISTING
    {
        std::printf("B: event() and fileFailure() return within 50 ms while the batch thread's listing is blocked for seconds; a failure filed meanwhile is sent\n");
        Rig rig("lock");
        rig.rep.fileFailure(failure("serial=HF0001A2"), kRing);   // something queued, urgent: the next poll starts a batch
        const auto mainId = std::this_thread::get_id();
        std::atomic<bool> release{false};
        std::atomic<int> blockedListings{0};
        Reporter::setDiskHookForTest([&] {
            if (std::this_thread::get_id() == mainId) { return; }   // the calling thread's own listings are not slow
            ++blockedListings;
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (!release.load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        });
        rig.rep.poll();   // starts the batch thread; its first listing blocks
        CHECK(waitFor([&] { return blockedListings.load() >= 1; }, 3000));
        RadioIdentity r = identify("airspyhf", "serial=HF0001A2", "Airspy HF+ (serial HF0001A2)");
        Event ev;
        ev.kind = "radio_open";
        ev.radio = r;
        ev.word = "start";
        ev.set("phase", "start").set("trigger", "startup-restore");
        const auto t0 = Clock::now();
        rig.rep.event(ev);
        const double eventMs = msSince(t0);
        const auto t1 = Clock::now();
        rig.rep.fileFailure(failure("serial=HF0001C4"), kRing);   // a failure written while the batch thread is mid-listing
        const double failMs = msSince(t1);
        std::printf("   event() %.1f ms, fileFailure() %.1f ms while the listing was blocked\n", eventMs, failMs);
        CHECK(eventMs < 50.0);
        CHECK(failMs < 50.0);
        release = true;
        // the failure filed meanwhile is sent in this session (the first batch lists after the block, the second is the bit's)
        CHECK(waitFor([&] { rig.rep.poll(); return rig.server->failures() == 2u; }, 8000));
        Reporter::setDiskHookForTest(nullptr);
    }

    return testSummary("test_radio_reporter_review");
}
