// test_radio_reporter_optout.cpp - THE RADIO REPORTER'S FILE CALLS THAT CAN FAIL (review of fix/freezes, must-fix 3). Three things, each with the failure staged by Reporter::setFileFaultHookForTest
// (a delete or a replace that fails as a sharing violation does while another thread has the file open):
//
//   A  THE OPT-OUT DELETE: Usage reporting OFF deletes every queued message that carries the install id. A delete that fails is held back from every listing and tried again at the next batch;
//      nothing carrying the install id is sent after the opt-out, however many times the delete fails, and the files are gone once it can succeed;
//   B  A BATCH THAT WAS RUNNING when the opt-out came (its lines were read before): the line in flight cannot be recalled, no other line is sent;
//   C  advance()'S RESULT: a position that cannot be written is kept in memory and the batch stops; nothing is sent twice, and the position lands when the file can be replaced;
//   D  TWO WRITERS NEVER COLLIDE ON ONE FILE NAME: the temporary name of the atomic write is unique to the call; many threads writing one file all succeed and leave one whole file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "test_check.hpp"

namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::int64_t kT0 = 1794830400;
const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

struct Bench {
    fs::path root;
    std::atomic<std::int64_t> clock{kT0};
    std::mutex mu;
    std::vector<json> sent;
    std::function<void()> onPost;   // called on the batch thread before each post is answered

    explicit Bench(const char* tag) {
        std::random_device rd;
        root = fs::temp_directory_path() / (std::string("rroptout-") + tag + "-" + std::to_string(rd()));
        fs::create_directories(root);
    }
    ~Bench() {
        rr::Reporter::setFileFaultHookForTest(nullptr);
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    rr::Reporter::Options options() {
        rr::Reporter::Options o;
        o.dir = (root / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.nowEpoch = [this]() { return clock.load(); };
        o.transport = [this](const std::string&, const std::string& body) {
            if (onPost) { onPost(); }
            std::lock_guard<std::mutex> lk(mu);
            sent.push_back(json::parse(body));
            return rr::PostResult{true, 204, 0, ""};
        };
        o.liveSend = true;
        o.identity.installId = kInstallId;
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 3;
        return o;
    }
    std::size_t sentOfKind(const char* kind) {
        std::lock_guard<std::mutex> lk(mu);
        std::size_t n = 0;
        for (const json& m : sent) { if (m.value("kind", std::string()) == kind) { ++n; } }
        return n;
    }
    std::size_t sentWithInstallId() {
        std::lock_guard<std::mutex> lk(mu);
        std::size_t n = 0;
        for (const json& m : sent) { if (m.dump().find(kInstallId) != std::string::npos) { ++n; } }
        return n;
    }
    std::size_t journalFiles() {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root / "radio-reports", ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("journal-", 0) == 0) { ++n; }
        }
        return n;
    }
    bool anyFileHoldsInstallId() {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!e.is_regular_file()) { continue; }
            std::ifstream f(e.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (text.find(kInstallId) != std::string::npos) { return true; }
        }
        return false;
    }
};

void queueOpen(rr::Reporter& rep, int n) {
    for (int i = 0; i < n; ++i) {
        rr::Event ev;
        ev.kind = "radio_open";
        ev.radio = rr::identify("rtlsdr", "serial=0000000" + std::to_string(i + 1), "RTL-SDR (serial 0000000" + std::to_string(i + 1) + ")");
        ev.word = "start";
        ev.set("phase", "start").set("trigger", "startup-restore");
        rep.event(ev);
    }
}

void identityOff(rr::Reporter& rep) {
    rr::Identity off = rep.identity();
    off.installId.clear();
    rep.setIdentity(off);
}

// A  a delete that fails: nothing carrying the install id is sent, the delete is retried
void testOptOutDeleteThatFails() {
    std::printf("A  an opt-out whose delete fails\n");
    Bench b("a");
    rr::Reporter::Options o = b.options();
    o.liveSend = false;   // queue only, until the opt-out is done
    rr::Reporter rep;
    rep.configure(o);
    queueOpen(rep, 3);
    CHECK(b.journalFiles() >= 1u);
    CHECK(b.anyFileHoldsInstallId());
    // the batch thread has the journal open: every delete of it fails
    std::atomic<int> failedDeletes{0};
    rr::Reporter::setFileFaultHookForTest([&](const char* op, const std::string& path) {
        const bool hit = std::string(op) == "remove" && path.find("journal-") != std::string::npos;
        if (hit) { ++failedDeletes; }
        return hit;
    });
    identityOff(rep);
    std::printf("    %d delete(s) refused; journal files still on disk: %zu\n", failedDeletes.load(), b.journalFiles());
    CHECK(failedDeletes.load() >= 1);
    CHECK(b.journalFiles() >= 1u);   // the file is still there: the delete failed
    // THE USER'S OPT-OUT IS HONOURED anyway: a live batch sends nothing that carries the install id
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    (void)rep.runBatchNow();
    std::printf("    messages with the install id sent after the opt-out: %zu; journal lines sent: %zu\n", b.sentWithInstallId(), b.sentOfKind("radio_open"));
    CHECK(b.sentWithInstallId() == 0u);
    CHECK(b.sentOfKind("radio_open") == 0u);
    // ...and the delete is done as soon as it can be
    rr::Reporter::setFileFaultHookForTest(nullptr);
    (void)rep.runBatchNow();
    CHECK(b.journalFiles() == 0u);
    CHECK(!b.anyFileHoldsInstallId());
    CHECK(b.sentWithInstallId() == 0u);
    rep.shutdown();
}

// B  a batch that read its lines before the opt-out sends none of the rest
void testOptOutDuringABatch() {
    std::printf("B  the opt-out arrives while a batch is sending\n");
    Bench b("b");
    rr::Reporter rep;
    rr::Reporter::Options o = b.options();
    o.liveSend = false;
    rep.configure(o);
    queueOpen(rep, 4);
    std::atomic<int> posts{0};
    b.onPost = [&]() {
        if (posts.fetch_add(1) == 0) { identityOff(rep); }   // the first post is in flight when Usage reporting goes off
    };
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    std::printf("    journal lines sent: %zu (one was in flight)\n", b.sentOfKind("radio_open"));
    CHECK(b.sentOfKind("radio_open") <= 1u);
    CHECK(posts.load() <= 1);
    CHECK(!b.anyFileHoldsInstallId());
    rep.shutdown();
}

// C  advance()'s result: a position that cannot be written
void testPositionThatCannotBeSaved() {
    std::printf("C  a position that cannot be written is not forgotten\n");
    Bench b("c");
    rr::Reporter rep;
    rr::Reporter::Options o = b.options();
    o.liveSend = false;
    rep.configure(o);
    queueOpen(rep, 3);
    rr::Reporter::setFileFaultHookForTest([](const char* op, const std::string& path) {
        return std::string(op) == "rename" && path.size() > 4 && path.compare(path.size() - 4, 4, ".pos") == 0;
    });
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    const std::size_t afterFirst = b.sentOfKind("radio_open");
    std::printf("    with the position file locked: %zu line(s) sent\n", afterFirst);
    CHECK(afterFirst == 1u);   // the batch stopped when it could not save its place
    rr::Reporter::setFileFaultHookForTest(nullptr);
    (void)rep.runBatchNow();
    (void)rep.runBatchNow();
    std::printf("    after the lock is gone: %zu line(s) sent in all\n", b.sentOfKind("radio_open"));
    CHECK(b.sentOfKind("radio_open") == 3u);   // each line exactly once: the first was not sent again
    rep.shutdown();
}

// D  two writers of one file
void testWritersNeverCollide() {
    std::printf("D  many writers of one file\n");
    Bench b("d");
    fs::create_directories(b.root / "radio-reports");
    const std::string path = ((b.root / "radio-reports") / "state.json").string();
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 150; ++i) {
                const std::string data = std::string(200 + static_cast<std::size_t>(t) * 37, static_cast<char>('a' + t));
                if (!rr::Reporter::writeAtomicForTest(path, data)) { ++failures; }
            }
        });
    }
    for (std::thread& th : ts) { th.join(); }
    std::printf("    %d write(s) failed of %d\n", failures.load(), 6 * 150);
    CHECK(failures.load() == 0);
    std::ifstream f(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool whole = !text.empty();
    for (char c : text) { if (c != text[0]) { whole = false; } }
    CHECK(whole);                                   // one writer's data, not a mixture
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(b.root / "radio-reports")) { (void)e; ++files; }
    CHECK(files == 1u);                             // no temporary file is left behind
}

}  // namespace

int main() {
    testOptOutDeleteThatFails();
    testOptOutDuringABatch();
    testPositionThatCannotBeSaved();
    testWritersNeverCollide();
    return testSummary("test_radio_reporter_optout");
}
