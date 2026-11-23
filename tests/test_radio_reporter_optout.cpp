// test_radio_reporter_optout.cpp - THE RADIO REPORTER'S FILE CALLS THAT CAN FAIL (review of fix/freezes, must-fix 3). Three things, each with the failure staged by Reporter::setFileFaultHookForTest
// (a delete or a replace that fails as a sharing violation does while another thread has the file open):
//
//   A  THE OPT-OUT DELETE: Usage reporting OFF deletes every queued message that carries the install id. A delete that fails is held back from every listing and tried again at the next batch;
//      nothing carrying the install id is sent after the opt-out, however many times the delete fails, and the files are gone once it can succeed;
//   B  A BATCH THAT WAS RUNNING when the opt-out came (its lines were read before): the line in flight cannot be recalled, no other line is sent;
//   C  advance()'S RESULT: a position that cannot be written is kept in memory and the batch stops; nothing is sent twice, and the position lands when the file can be replaced;
//   D  TWO WRITERS NEVER COLLIDE ON ONE FILE NAME: the temporary name of the atomic write is unique to the call; many threads writing one file all succeed and leave one whole file;
//   E  THE OPT-OUT ACROSS A RESTART (review of fix/freezes part 3, F5): Usage reporting OFF, the delete fails, the program quits, starts again: nothing queued under the install id is sent, with the
//      delete still failing and when it can succeed; a delete that fails is retried at quit;
//   F  AN ID CHANGE: a line stored under another install id is never posted under the current one;
//   G  A FAILURE READ BEFORE THE OPT-OUT is posted in the anonymous form (no install id, no session id, no seq, a message id), never with the id; a failure file that could not be rewritten at the
//      opt-out is rewritten at the next start. The failure itself is always sent (the owner's rule of 2026-11-15).
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

rr::FailureInput failureOf(const char* serial) {
    rr::FailureInput f;
    f.driverKind = "rtlsdr";
    f.args = std::string("serial=") + serial;
    f.nativeLabel = std::string("RTL-SDR (serial ") + serial + ")";
    f.reason = "usb_open error -3";
    f.reasonClass = "busy";
    f.trigger = rr::Trigger::StartupRestore;
    return f;
}

const std::vector<std::string> kNoRing = {};

std::size_t sentWithoutInstallIdOfKind(Bench& b, const char* kind) {
    std::lock_guard<std::mutex> lk(b.mu);
    std::size_t n = 0;
    for (const json& m : b.sent) {
        if (m.value("kind", std::string()) == kind && !m.contains("installId") && !m.contains("sessionId") && !m.contains("seq") && m.contains("messageId")) { ++n; }
    }
    return n;
}

// E  the opt-out across a restart
void testOptOutSurvivesARestart() {
    std::printf("E  the opt-out across a restart\n");
    Bench b("e");
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.liveSend = false;
        rep.configure(o);
        queueOpen(rep, 3);
        CHECK(b.journalFiles() >= 1u);
        // every delete of a journal file fails (a scanner has it open), through the opt-out AND the quit
        rr::Reporter::setFileFaultHookForTest([](const char* op, const std::string& path) { return std::string(op) == "remove" && path.find("journal-") != std::string::npos; });
        identityOff(rep);
        CHECK(b.journalFiles() >= 1u);   // still on disk
        rep.flushAtQuit(200);            // the retry at quit fails too
        CHECK(b.journalFiles() >= 1u);
    }                                    // the program ends with the segment on disk
    // START AGAIN with Usage reporting off and the delete STILL failing: nothing queued before the opt-out is sent.
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.identity.installId.clear();
        rep.configure(o);
        (void)rep.runBatchNow();
        (void)rep.runBatchNow();
        std::printf("    restart, delete still failing: lines sent %zu, with the id %zu\n", b.sentOfKind("radio_open"), b.sentWithInstallId());
        CHECK(b.sentOfKind("radio_open") == 0u);
        CHECK(b.sentWithInstallId() == 0u);
        rep.shutdown();
    }
    // ...and with the delete able to succeed the next start removes the segment.
    rr::Reporter::setFileFaultHookForTest(nullptr);
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.identity.installId.clear();
        rep.configure(o);
        (void)rep.runBatchNow();
        CHECK(b.sentOfKind("radio_open") == 0u);
        CHECK(b.journalFiles() == 0u);
        CHECK(!b.anyFileHoldsInstallId());
        rep.shutdown();
    }
}

void testDeferredDeleteIsRetriedAtQuit() {
    std::printf("E2 a delete that failed at the opt-out is retried at quit\n");
    Bench b("e2");
    rr::Reporter rep;
    rr::Reporter::Options o = b.options();
    o.liveSend = false;
    rep.configure(o);
    queueOpen(rep, 2);
    std::atomic<bool> locked{true};
    rr::Reporter::setFileFaultHookForTest([&](const char* op, const std::string& path) { return locked.load() && std::string(op) == "remove" && path.find("journal-") != std::string::npos; });
    identityOff(rep);
    CHECK(b.journalFiles() >= 1u);
    locked.store(false);                 // the scanner let go; nothing else runs a batch: only the quit can do it
    rep.flushAtQuit(200);
    std::printf("    journal files after the quit: %zu\n", b.journalFiles());
    CHECK(b.journalFiles() == 0u);
    rr::Reporter::setFileFaultHookForTest(nullptr);
}

// F  an id change
void testAnIdChangeMakesOlderLinesUnsendable() {
    std::printf("F  an id change\n");
    Bench b("f");
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.liveSend = false;
        rep.configure(o);
        queueOpen(rep, 3);
        rep.flushAtQuit(100);
    }
    const char* const kOtherId = "0f0e0d0c0b0a09080706050403020100";
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.identity.installId = kOtherId;
        o.sessionId = "bbbbbbbbbbbbbbbb";
        rep.configure(o);
        queueOpen(rep, 1);                 // a line of THIS id, in a segment of its own
        (void)rep.runBatchNow();
        (void)rep.runBatchNow();
        std::printf("    lines sent %zu (1 expected), with the old id %zu, unsendable %llu\n", b.sentOfKind("radio_open"), b.sentWithInstallId(), static_cast<unsigned long long>(rep.stats().unsendable));
        CHECK(b.sentOfKind("radio_open") == 1u);       // only the line of the current id
        CHECK(b.sentWithInstallId() == 0u);            // nothing under the old id
        CHECK(rep.stats().unsendable == 3u);
        rep.shutdown();
    }
}

// G  a failure read before the opt-out, and a failure file that could not be rewritten
void testAFailureReadBeforeTheOptOutIsPostedAnonymously() {
    std::printf("G  a failure read before the opt-out\n");
    Bench b("g");
    rr::Reporter rep;
    rr::Reporter::Options o = b.options();
    o.liveSend = false;
    rep.configure(o);
    rep.fileFailure(failureOf("00000001"), kNoRing);
    rep.fileFailure(failureOf("00000002"), kNoRing);
    rep.fileFailure(failureOf("00000003"), kNoRing);
    CHECK(b.anyFileHoldsInstallId());
    std::atomic<int> posts{0};
    b.onPost = [&]() {
        if (posts.fetch_add(1) == 0) { identityOff(rep); }   // the first post is in flight; the other two were READ with the id
    };
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    std::printf("    failures sent: %zu, of which anonymous %zu, with the id %zu\n", b.sentOfKind("radio_fail"), sentWithoutInstallIdOfKind(b, "radio_fail"), b.sentWithInstallId());
    CHECK(b.sentOfKind("radio_fail") == 3u);                       // every failure still reaches the owner
    CHECK(sentWithoutInstallIdOfKind(b, "radio_fail") == 2u);      // the two read before the opt-out go without the id; the one in flight could not be recalled
    CHECK(b.sentWithInstallId() == 1u);
    CHECK(!b.anyFileHoldsInstallId());
    rep.shutdown();
}

void testAFailureFileThatCouldNotBeRewrittenIsFixedAtTheNextStart() {
    std::printf("G2 a failure file that could not be rewritten at the opt-out\n");
    Bench b("g2");
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.liveSend = false;
        rep.configure(o);
        rep.fileFailure(failureOf("00000004"), kNoRing);
        // the REPLACE of a failure file fails at the opt-out (the file is open elsewhere: neither a rename over it nor a delete of it can be done)
        rr::Reporter::setFileFaultHookForTest([](const char*, const std::string& path) { return path.find("fail-") != std::string::npos; });
        identityOff(rep);
        CHECK(rep.stats().guardRefused >= 1u);       // the failed rewrite is counted, not ignored
        CHECK(b.anyFileHoldsInstallId());            // it is still on disk with the id...
        rr::Reporter::setFileFaultHookForTest(nullptr);
    }                                                // ...and the program ends
    rr::Reporter rep;
    rr::Reporter::Options o = b.options();
    o.identity.installId.clear();
    rep.configure(o);                                // the start with Usage reporting off rewrites it
    CHECK(!b.anyFileHoldsInstallId());
    (void)rep.runBatchNow();
    CHECK(b.sentOfKind("radio_fail") == 1u);
    CHECK(sentWithoutInstallIdOfKind(b, "radio_fail") == 1u);
    rep.shutdown();
}

// G3 (final review of fix/freezes, R4): the anonymous REWRITE of a queued failure file must never delete the failure when the atomic replace fails. Only the RENAME is made to fail
// (the temporary file is held by a scanner while the target is free): the old delete-then-rename fallback removed the target, found the rename failing and removed the temporary file too,
// so the report was gone. Now the original stays where it is, carrying the id, the send-time rule posts it without the id, and the next start rewrites it.
std::size_t failFilesOnDisk(Bench& b) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(b.root / "radio-reports", ec)) {
        if (e.path().filename().string().rfind("fail-", 0) == 0 && e.path().extension() == ".json") { ++n; }
    }
    return n;
}

void testARewriteThatCannotRenameNeverDeletesTheFailure() {
    std::printf("G3 a rewrite whose rename fails keeps the failure report\n");
    Bench b("g3");
    {
        rr::Reporter rep;
        rr::Reporter::Options o = b.options();
        o.liveSend = false;
        rep.configure(o);
        rep.fileFailure(failureOf("00000005"), kNoRing);
        CHECK(failFilesOnDisk(b) == 1u);
        rr::Reporter::setFileFaultHookForTest([](const char* op, const std::string& path) { return std::string(op) == "rename" && path.find("fail-") != std::string::npos; });
        identityOff(rep);
        CHECK(rep.stats().guardRefused >= 1u);       // the failed rewrite is counted
        CHECK(failFilesOnDisk(b) == 1u);             // THE REPORT IS STILL THERE (the delete-then-rename fallback made this 0)
        CHECK(b.anyFileHoldsInstallId());            // as the original, with the id, which the send-time rule strips
        // a batch while the rename still fails: the failure is posted, anonymously
        rep.setLiveSend(true);
        (void)rep.runBatchNow();
        CHECK(b.sentOfKind("radio_fail") == 1u);
        CHECK(sentWithoutInstallIdOfKind(b, "radio_fail") == 1u);
        CHECK(b.sentWithInstallId() == 0u);
        rr::Reporter::setFileFaultHookForTest(nullptr);
        rep.shutdown();
    }
    // and a failure that is NOT sent (live sending off) is rewritten at the next start, once the files can be replaced
    Bench c("g3b");
    {
        rr::Reporter rep;
        rr::Reporter::Options o = c.options();
        o.liveSend = false;
        rep.configure(o);
        rep.fileFailure(failureOf("00000006"), kNoRing);
        rr::Reporter::setFileFaultHookForTest([](const char* op, const std::string& path) { return std::string(op) == "rename" && path.find("fail-") != std::string::npos; });
        identityOff(rep);
        CHECK(failFilesOnDisk(c) == 1u);
        rr::Reporter::setFileFaultHookForTest(nullptr);
    }
    rr::Reporter rep;
    rr::Reporter::Options o = c.options();
    o.identity.installId.clear();
    rep.configure(o);
    CHECK(failFilesOnDisk(c) == 1u);
    CHECK(!c.anyFileHoldsInstallId());               // retried at the next start, now rewritten
    rep.shutdown();
}

}  // namespace

int main() {
    testOptOutDeleteThatFails();
    testOptOutDuringABatch();
    testPositionThatCannotBeSaved();
    testWritersNeverCollide();
    testOptOutSurvivesARestart();
    testDeferredDeleteIsRetriedAtQuit();
    testAnIdChangeMakesOlderLinesUnsendable();
    testAFailureReadBeforeTheOptOutIsPostedAnonymously();
    testAFailureFileThatCouldNotBeRewrittenIsFixedAtTheNextStart();
    testARewriteThatCannotRenameNeverDeletesTheFailure();
    return testSummary("test_radio_reporter_optout");
}
