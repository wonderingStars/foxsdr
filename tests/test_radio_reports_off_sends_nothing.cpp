// USAGE REPORTING OFF SENDS NOTHING (owner's decision of 2026-11-24, brief 62; it reverses decision 12 of docs/radio-reports-contract.md, "a radio failure report is always sent").
//
// With Usage reporting off the reporter must not build, queue, write or send a radio failure report - on any path that reaches the sender - and switching it off deletes every
// failure report already queued, like the journal. With it on, the failure report is exactly what 0.99.114 sent. Every case below is named for the path it covers:
//
//   A  OFF: every trigger, a retry inside a series, a superseded open, the first-session record a failure raises on a first launch, the journal, the urgent poll, the batch and the quit:
//      no file on disk, no post, no counter raised (the first failure after switching ON is attempt 1)
//   B  ON: the failure report is what it was (envelope and body keys and values pinned)
//   C  a queue holding failures when Usage reporting goes OFF: the files are gone; ON again: one new failure is reported and the old ones do not reappear
//   D1 what is filed before the queue is opened (the start-up restore runs first) is dropped when the queue opens with Usage reporting off
//   D2 failure files left by an earlier version (0.99.109 to 0.99.114 queued an ANONYMOUS failure with Usage reporting off): never sent, deleted at the start
//   D3 the degraded resend: an opt-out that arrives while a failure is in flight and refused for its text does not make a second post
//   D4 an opt-out that arrives while a batch has read several failures: none of the rest is posted
//   E1 a failure held by the daily bound is forgotten by a switch-off (and by a start with Usage already off); E2 OFF then ON during a batch; E3 a failure of another install id is never sent (review 62)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <string>
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
const char* const kOtherId = "0f0e0d0c0b0a09080706050403020100";

struct Bench {
    fs::path root;
    std::atomic<std::int64_t> clock{kT0};
    std::mutex mu;
    std::vector<json> sent;
    std::function<rr::PostResult()> answer;   // default: 204

    explicit Bench(const char* tag) {
        std::random_device rd;
        root = fs::temp_directory_path() / (std::string("rroff-") + tag + "-" + std::to_string(rd()));
        fs::create_directories(root);
    }
    ~Bench() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    rr::Reporter::Options options(bool usageOn, bool liveSend = true, std::uint64_t launches = 3) {
        rr::Reporter::Options o;
        o.dir = (root / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.nowEpoch = [this]() { return clock.load(); };
        o.transport = [this](const std::string&, const std::string& body) {
            std::function<rr::PostResult()> a;
            {
                std::lock_guard<std::mutex> lk(mu);
                sent.push_back(json::parse(body));
                a = answer;
            }
            return a ? a() : rr::PostResult{true, 204, 0, ""};
        };
        o.liveSend = liveSend;
        if (usageOn) { o.identity.installId = kInstallId; }
        o.identity.version = "0.99.115";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.channel = "installer";
        o.identity.updateChannel = "stable";
        o.identity.launches = launches;
        return o;
    }
    std::string dir() const { return (root / "radio-reports").string(); }
    std::size_t posts() {
        std::lock_guard<std::mutex> lk(mu);
        return sent.size();
    }
    std::size_t sentOfKind(const char* kind) {
        std::lock_guard<std::mutex> lk(mu);
        std::size_t n = 0;
        for (const json& m : sent) { if (m.value("kind", std::string()) == kind) { ++n; } }
        return n;
    }
    std::vector<fs::path> failFiles() const {
        std::vector<fs::path> out;
        std::error_code ec;
        for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind("fail-", 0) == 0 && it->path().extension() == ".json") { out.push_back(it->path()); }
        }
        return out;
    }
    // every file of the folder, as one string: nothing about a failure may be anywhere in it
    std::string everything() const {
        std::string all;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file()) { continue; }
            std::ifstream f(it->path(), std::ios::binary);
            all.append(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        return all;
    }
};

rr::FailureInput failureOf(const char* serial, rr::Trigger t = rr::Trigger::StartupRestore, const char* cls = "busy") {
    rr::FailureInput f;
    f.driverKind = "rtlsdr";
    f.args = std::string("serial=") + serial;
    f.nativeLabel = std::string("RTL-SDR (serial ") + serial + ")";
    f.reason = "the radio is in use by another program";
    f.reasonClass = cls;
    f.trigger = t;
    return f;
}

const std::vector<std::string> kRing = {"12:01:07.331 warn rtlsdr: open failed"};

void identityOff(rr::Reporter& rep) {
    rr::Identity id = rep.identity();
    id.installId.clear();
    rep.setIdentity(id);
}

void identityOn(rr::Reporter& rep, const char* installId) {
    rr::Identity id = rep.identity();
    id.installId = installId;
    rep.setIdentity(id);
}

json readFailFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return json::parse(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()), nullptr, false);
}

std::set<std::string> keysOf(const json& o) {
    std::set<std::string> k;
    for (auto it = o.begin(); it != o.end(); ++it) { k.insert(it.key()); }
    return k;
}

// A  Usage reporting OFF: every way a failure can arrive, and everything that could send one
void testOffSendsNothing() {
    std::printf("A  Usage reporting off: no failure report on any trigger, nothing on disk, nothing posted, no counter raised\n");
    Bench b("a");
    rr::Reporter rep;
    rep.configure(b.options(/*usageOn=*/false, /*liveSend=*/true, /*launches=*/1));   // a FIRST launch: a failure would also raise the first-session record
    for (const rr::Trigger t : {rr::Trigger::StartupRestore, rr::Trigger::UserPick, rr::Trigger::SavedRetry, rr::Trigger::PatchRadio, rr::Trigger::AutoReopen, rr::Trigger::Other}) {
        rep.fileFailure(failureOf("00000001", t), kRing);
    }
    rr::FailureInput retry = failureOf("00000001");
    retry.countOnly = true;                      // a retry inside a series: only the ordinal moves
    rep.fileFailure(retry, kRing);
    rr::FailureInput sup = failureOf("00000002");
    sup.superseded = true;
    rep.fileFailure(sup, kRing);
    rep.firstSession("{\"v\":\"0.99.115\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1}", "on-failure");   // the record a failure raises on a first launch
    rep.firstSession("{\"v\":\"0.99.115\",\"launches\":1}", "after-minute");
    rr::Event ev;
    ev.kind = "radio_open";
    ev.radio = rr::identify("rtlsdr", "serial=00000001", "RTL-SDR (serial 00000001)");
    ev.word = "start";
    ev.set("phase", "start").set("trigger", "startup-restore");
    rep.event(ev);
    b.clock += 400;
    rep.poll();                                  // the urgent poll the engine's pass makes
    rep.setLiveSend(true);
    CHECK(rep.runBatchNow() == 0);               // a batch
    rep.flushAtQuit(300);                        // the send at quit
    rep.shutdown();                              // joins a batch thread the poll may have started
    CHECK(b.posts() == 0);
    CHECK(b.failFiles().empty());
    CHECK(rep.stats().failuresQueued == 0 && rep.stats().journalQueued == 0 && rep.stats().posts == 0);
    CHECK(b.everything().find("radio_fail") == std::string::npos);
    CHECK(b.everything().find("first_session") == std::string::npos);
    // no failure counter was raised for the failures above: the first failure after switching ON is attempt 1, not attempt 9
    rr::Reporter again;
    again.configure(b.options(false, true, 3));
    identityOn(again, kInstallId);
    again.fileFailure(failureOf("00000001"), kRing);
    const std::vector<fs::path> ff = b.failFiles();
    CHECK(ff.size() == 1);
    if (ff.size() == 1) {
        const json w = readFailFile(ff[0]);
        CHECK(w.is_object() && w["msg"]["body"]["attempt"] == 1);
    }
    again.shutdown();
}

// B  Usage reporting ON: the failure report is what 0.99.114 sent
void testOnIsUnchanged() {
    std::printf("B  Usage reporting on: the failure report is unchanged\n");
    Bench b("b");
    rr::Reporter rep;
    rep.configure(b.options(true, true, 3));
    rep.setDiagOn(true);
    rr::FailureInput f = failureOf("00000001", rr::Trigger::UserPick, "busy");
    f.openMs = 1500;
    f.sampleRateSps = 2048000;
    rep.fileFailure(f, kRing);
    CHECK(rep.stats().failuresQueued == 1);
    const std::vector<fs::path> ff = b.failFiles();
    CHECK(ff.size() == 1);
    if (ff.size() == 1) {
        const json w = readFailFile(ff[0]);
        const json& m = w["msg"];
        CHECK((keysOf(m) == std::set<std::string>{"schema", "kind", "installId", "sessionId", "seq", "t", "radioKey", "body"}));
        CHECK(m["kind"] == "radio_fail" && m["installId"] == kInstallId && m.contains("sessionId") && m.contains("seq") && !m.contains("messageId"));
        const json& body = m["body"];
        CHECK(body["driver"] == "rtlsdr" && body["serial"] == "00000001" && body["reasonClass"] == "busy" && body["trigger"] == "user-pick");
        CHECK(body["attempt"] == 1 && body["openMs"] == 1500 && body["sampleRateSps"] == 2048000 && body["superseded"] == false && body["diagOn"] == true);
        CHECK(!body["logLines"].empty() && body["v"] == "0.99.115" && body["ch"] == "installer" && body["uch"] == "stable");
        CHECK(body["os"] == "Windows 10.0.22631" && body["arch"] == "x64");
        const std::set<std::string> want = {"driver", "model", "serial", "reason", "reasonClass", "trigger", "attempt", "openMs", "sampleRateSps",
                                            "superseded", "diagOn", "logLines", "v", "ch", "uch", "os", "arch"};
        CHECK(keysOf(body) == want);
    }
    CHECK(rep.runBatchNow() == 1);                // sent, with the id, and the file is gone
    CHECK(b.sentOfKind("radio_fail") == 1);
    {
        std::lock_guard<std::mutex> lk(b.mu);
        CHECK(b.sent.size() == 1 && b.sent[0]["installId"] == kInstallId);
    }
    CHECK(b.failFiles().empty());
    rep.shutdown();
}

// C  queued failures when Usage reporting goes OFF, then ON again
void testQueuedFailuresGoWhenSwitchedOff() {
    std::printf("C  failures queued when Usage reporting goes off are deleted; on again, a new failure is reported and the old ones do not reappear\n");
    Bench b("c");
    rr::Reporter rep;
    rep.configure(b.options(true, /*liveSend=*/false, 3));
    rep.fileFailure(failureOf("00000001"), kRing);
    rep.fileFailure(failureOf("00000002", rr::Trigger::SavedRetry), kRing);
    rep.fileFailure(failureOf("00000003", rr::Trigger::UserPick, "control"), kRing);
    CHECK(b.failFiles().size() == 3);
    identityOff(rep);
    CHECK(b.failFiles().empty());                                           // gone at once, like the journal
    CHECK(b.everything().find(kInstallId) == std::string::npos);
    rep.fileFailure(failureOf("00000004"), kRing);                          // a failure while off: nothing
    CHECK(b.failFiles().empty());
    identityOn(rep, kOtherId);                                              // switched on again (a new install id is minted)
    CHECK(b.failFiles().empty());                                           // nothing came back
    rep.fileFailure(failureOf("00000005"), kRing);
    CHECK(b.failFiles().size() == 1);
    rep.setLiveSend(true);
    CHECK(rep.runBatchNow() == 1);
    CHECK(b.sentOfKind("radio_fail") == 1);
    {
        std::lock_guard<std::mutex> lk(b.mu);
        CHECK(b.sent[0]["body"]["serial"] == "00000005" && b.sent[0]["installId"] == kOtherId);
    }
    CHECK(b.failFiles().empty());
    rep.shutdown();
}

// D1 what was filed before the queue could be opened
void testPreConfigureBufferWithUsageOff() {
    std::printf("D1 failures filed before the queue is opened are dropped when it opens with Usage reporting off, kept when it opens with it on\n");
    {
        Bench b("d1off");
        rr::Reporter rep;
        rep.fileFailure(failureOf("00000001"), kRing);      // before configure: kept in memory
        rep.fileFailure(failureOf("00000002", rr::Trigger::UserPick), kRing);
        rep.configure(b.options(false, true, 1));
        CHECK(b.failFiles().empty());
        CHECK(rep.runBatchNow() == 0 && b.posts() == 0);
        CHECK(b.everything().find("radio_fail") == std::string::npos);
        rep.shutdown();
    }
    {
        Bench b("d1on");                                    // the control: the same with Usage reporting on is filed
        rr::Reporter rep;
        rep.fileFailure(failureOf("00000001"), kRing);
        rep.configure(b.options(true, true, 3));
        CHECK(b.failFiles().size() == 1);
        rep.shutdown();
    }
}

// the first-session record a failure raises on a first launch (control: with Usage reporting on it is queued)
void testFirstSessionOnFailure() {
    std::printf("D1b the first-session record a failure raises on a first launch: queued with Usage reporting on, never with it off\n");
    {
        Bench b("d1b-on");
        rr::Reporter rep;
        rep.configure(b.options(true, false, 1));
        rep.firstSession("{\"v\":\"0.99.115\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1}", "on-failure");
        CHECK(b.everything().find("first_session") != std::string::npos);
        rep.shutdown();
    }
    {
        Bench b("d1b-off");
        rr::Reporter rep;
        rep.configure(b.options(false, false, 1));
        rep.firstSession("{\"v\":\"0.99.115\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1}", "on-failure");
        CHECK(b.everything().find("first_session") == std::string::npos);
        rep.shutdown();
    }
}

// D2 failure files an earlier version left
void testFilesFromEarlierVersionsAreNeverSent() {
    std::printf("D2 failure files left by 0.99.109-0.99.114 (with the id, and the anonymous form queued with Usage reporting off) are never sent and are deleted\n");
    Bench b("d2");
    {
        rr::Reporter rep;
        rep.configure(b.options(true, false, 3));
        rep.fileFailure(failureOf("00000001"), kRing);
        rep.fileFailure(failureOf("00000002"), kRing);
        rep.shutdown();
    }
    // make the second file the anonymous form an install with Usage reporting off queued in 0.99.114
    std::vector<fs::path> ff = b.failFiles();
    CHECK(ff.size() == 2);
    if (ff.size() == 2) {
        json w = readFailFile(ff[1]);
        w["msg"].erase("installId");
        w["msg"].erase("sessionId");
        w["msg"].erase("seq");
        w["msg"]["messageId"] = "0123456789abcdef";
        std::ofstream o(ff[1], std::ios::binary | std::ios::trunc);
        o << w.dump();
    }
    {
        rr::Reporter rep;
        rep.configure(b.options(false, true, 3));          // the start with Usage reporting OFF
        CHECK(b.failFiles().empty());                      // both are deleted before any batch can run
        CHECK(rep.runBatchNow() == 0);
        rep.poll();
        rep.flushAtQuit(300);
        rep.shutdown();
        CHECK(b.posts() == 0);
    }
    // the SAME seed with Usage reporting ON: the file that carries THIS install's id is sent as today; the file of the old anonymous form was queued during an OFF period and is never sent,
    // even after Usage reporting is on (review 62, should-fix 2: after OFF then ON the first thing a report can describe is a failure that happened after the ON)
    Bench c("d2on");
    {
        rr::Reporter rep;
        rep.configure(c.options(true, false, 3));
        rep.fileFailure(failureOf("00000001"), kRing);
        rep.fileFailure(failureOf("00000002"), kRing);
        rep.shutdown();
    }
    {
        std::vector<fs::path> cf = c.failFiles();
        CHECK(cf.size() == 2);
        if (cf.size() == 2) {
            json w = readFailFile(cf[1]);
            w["msg"].erase("installId");
            w["msg"].erase("sessionId");
            w["msg"].erase("seq");
            w["msg"]["messageId"] = "0123456789abcdef";
            std::ofstream o(cf[1], std::ios::binary | std::ios::trunc);
            o << w.dump();
        }
        rr::Reporter rep;
        rep.configure(c.options(true, true, 3));
        CHECK(c.failFiles().size() == 2);
        CHECK(rep.runBatchNow() == 1);
        CHECK(c.sentOfKind("radio_fail") == 1);
        CHECK(c.failFiles().empty());                      // the anonymous-form file is consumed unsent
        rep.shutdown();
    }
}

// D3 the degraded resend
void testDegradedResendAfterOptOut() {
    std::printf("D3 an opt-out while a failure is in flight and refused for its text: no degraded resend\n");
    Bench b("d3");
    rr::Reporter rep;
    rep.configure(b.options(true, false, 3));
    rep.fileFailure(failureOf("00000001"), kRing);
    CHECK(b.failFiles().size() == 1);
    std::atomic<int> calls{0};
    b.answer = [&]() {
        if (calls.fetch_add(1) == 0) { identityOff(rep); }   // Usage reporting is switched off while the first post is out; the answer asks for the text to be withheld
        return rr::PostResult{true, 400, 0, "refused: path-in-text"};
    };
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    CHECK(b.posts() == 1);                                   // the resend of the degraded form is never made
    CHECK(rep.stats().degradedResends == 0);
    CHECK(b.failFiles().empty());
    CHECK(b.everything().find("radio_fail") == std::string::npos);
    rep.shutdown();
}

// D4 an opt-out while a batch has read several failures
void testOptOutDuringABatchOfFailures() {
    std::printf("D4 an opt-out while a batch holds several failures: only the one in flight is posted\n");
    Bench b("d4");
    rr::Reporter rep;
    rep.configure(b.options(true, false, 3));
    for (const char* s : {"00000001", "00000002", "00000003", "00000004"}) { rep.fileFailure(failureOf(s), kRing); }
    std::atomic<int> calls{0};
    b.answer = [&]() {
        if (calls.fetch_add(1) == 0) { identityOff(rep); }
        return rr::PostResult{true, 204, 0, ""};
    };
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    (void)rep.runBatchNow();
    CHECK(b.posts() == 1);
    CHECK(b.failFiles().empty());
    rep.shutdown();
}

// E1 review 62, should-fix 1: a failure the daily bound held back must not survive a switch-off
void testHeldFailuresDoNotSurviveASwitchOff() {
    std::printf("E1 a failure held by the daily bound is forgotten by a switch-off: never filed, never sent after switching on again\n");
    for (int variant = 0; variant < 2; ++variant) {
        Bench b(variant == 0 ? "e1a" : "e1b");
        rr::Reporter rep;
        rr::Reporter::Options o = b.options(true, /*liveSend=*/false, 3);
        o.failuresPerDay = 1;
        rep.configure(o);
        rep.fileFailure(failureOf("00000007"), kRing);                 // admitted: the first of its radio and class
        rr::FailureInput held = failureOf("00000007", rr::Trigger::UserPick);
        held.reason = "HELDSECRET text of the held failure";
        rep.fileFailure(held, kRing);                                  // the same radio and class again: over the bound of one, so it is held
        CHECK(rep.heldCount() == 1);
        CHECK(b.everything().find("HELDSECRET") != std::string::npos); // it is in state.json
        if (variant == 1) { rep.shutdown(); }                          // b: the run ends; the next start loads state.json
        rr::Reporter again;
        rr::Reporter& r = variant == 0 ? rep : again;
        if (variant == 1) {
            rr::Reporter::Options o2 = b.options(false, true, 3);     // the next start finds Usage reporting OFF
            o2.failuresPerDay = 1;
            r.configure(o2);
        } else {
            identityOff(r);                                            // a: switched off while running
        }
        CHECK(r.heldCount() == 0);                                     // forgotten at once
        CHECK(b.everything().find("HELDSECRET") == std::string::npos); // and gone from state.json
        r.setLiveSend(true);
        b.clock += 3 * 86400;                                          // a day rolls over while still OFF
        r.poll();
        r.flushAtQuit(300);                                            // the quit flush used to file the held failure as an anonymous file
        CHECK(b.failFiles().empty());
        CHECK(b.posts() == 0);
        identityOn(r, kOtherId);                                       // switched on again
        b.clock += 3 * 86400;                                          // the day rolls over: this is when releaseHeld ran
        r.fileFailure(failureOf("00000007"), kRing);                   // a new failure, same radio: the first thing a report can describe
        r.flushAtQuit(300);
        r.shutdown();
        const std::vector<fs::path> ff = b.failFiles();
        std::size_t radioFails = 0;
        {
            std::lock_guard<std::mutex> lk(b.mu);
            for (const json& m : b.sent) {
                if (m.value("kind", std::string()) != "radio_fail") { continue; }
                ++radioFails;
                CHECK(m["body"]["reason"] != "HELDSECRET text of the held failure");
                CHECK(m["body"]["attempt"] == 1);                      // the attempt counter started again
                CHECK(!m["body"].contains("dropped"));
            }
        }
        CHECK(radioFails + ff.size() == 1);                            // exactly one failure exists after the ON: the new one
        CHECK(b.everything().find("HELDSECRET") == std::string::npos);
    }
}

// E2 review 62, should-fix 2: OFF then ON while a batch is being posted
void testOffOnDuringABatchSendsNothingFromBefore() {
    std::printf("E2 switch off and on again while a batch is in flight: what the batch had read before the switch-off is not posted\n");
    Bench b("e2");
    rr::Reporter rep;
    rep.configure(b.options(true, false, 3));
    for (const char* s : {"00000001", "00000002", "00000003"}) { rep.fileFailure(failureOf(s), kRing); }
    CHECK(b.failFiles().size() == 3);
    std::atomic<int> calls{0};
    b.answer = [&]() {
        if (calls.fetch_add(1) == 0) { identityOff(rep); identityOn(rep, kOtherId); }   // OFF then ON (a new id is minted) while the first post is out
        return rr::PostResult{true, 204, 0, ""};
    };
    rep.setLiveSend(true);
    (void)rep.runBatchNow();
    (void)rep.runBatchNow();
    CHECK(b.posts() == 1);                                   // only the post that was already in flight
    CHECK(b.failFiles().empty());
    rep.fileFailure(failureOf("00000009"), kRing);           // the first thing after the ON
    CHECK(rep.runBatchNow() == 1);
    CHECK(b.posts() == 2);
    rep.shutdown();
}

// E3 the same root, narrower: a queued failure that carries an install id that is not the current one
void testFailureOfAnotherInstallIdIsNeverSent() {
    std::printf("E3 a queued failure that carries an install id that is not the current one is consumed unsent (no anonymous re-issue)\n");
    Bench b("e3");
    {
        rr::Reporter rep;
        rep.configure(b.options(true, false, 3));
        rep.fileFailure(failureOf("00000001"), kRing);
        rep.fileFailure(failureOf("00000002"), kRing);
        rep.shutdown();
    }
    CHECK(b.failFiles().size() == 2);
    rr::Reporter rep;
    rr::Reporter::Options o = b.options(true, true, 3);
    o.identity.installId = kOtherId;                         // a start after an OFF period: the id is a new one
    rep.configure(o);
    CHECK(rep.runBatchNow() == 0);
    CHECK(b.posts() == 0);
    CHECK(b.failFiles().empty());
    rep.shutdown();
}

// F1 review 79, M5: a failed rewrite of state.json at the switch-off must not leave the held failure in it
void testFailedStateRewriteAtSwitchOffDeletesTheFile() {
    std::printf("F1 the rewrite of state.json at the switch-off fails: the file is deleted (retried at the next batch) and holds nothing for a later start\n");
    Bench b("f1");
    rr::Reporter rep;
    rr::Reporter::Options o = b.options(true, false, 3);
    o.failuresPerDay = 1;
    rep.configure(o);
    rep.fileFailure(failureOf("00000007"), kRing);
    rr::FailureInput held = failureOf("00000007", rr::Trigger::UserPick);
    held.reason = "HELDSECRET text of the held failure";
    rep.fileFailure(held, kRing);
    CHECK(rep.heldCount() == 1);
    CHECK(b.everything().find("HELDSECRET") != std::string::npos);
    std::atomic<bool> fault{true};
    rr::Reporter::setFileFaultHookForTest([&](const char* op, const std::string& path) {
        (void)op;
        return fault.load() && path.find("state.json") != std::string::npos;   // neither the replace nor the delete of state.json works
    });
    identityOff(rep);                                          // the rewrite fails; the delete cannot be done now either
    CHECK(rep.heldCount() == 0);
    fault = false;                                             // the sharing violation ends
    rep.setLiveSend(true);
    (void)rep.runBatchNow();                                   // the next batch retries the delete
    CHECK(b.everything().find("HELDSECRET") == std::string::npos);
    rr::Reporter::setFileFaultHookForTest(nullptr);
    rep.shutdown();
}

// F2 review 79, M8: a held failure written by an older version (no install id in state.json) is not released at the first start of this one
void testStateOfAnOlderVersionIsNotReleased() {
    std::printf("F2 a held failure in a state.json of an older version (no id) is dropped at the first start with Usage ON; one this version wrote, with the same id, is kept\n");
    for (int variant = 0; variant < 2; ++variant) {
        Bench b(variant == 0 ? "f2a" : "f2b");
        {
            rr::Reporter rep;
            rr::Reporter::Options o = b.options(true, false, 3);
            o.failuresPerDay = 1;
            rep.configure(o);
            rep.fileFailure(failureOf("00000007"), kRing);
            rr::FailureInput held = failureOf("00000007", rr::Trigger::UserPick);
            held.reason = "HELDSECRET text of the held failure";
            rep.fileFailure(held, kRing);
            CHECK(rep.heldCount() == 1);
            rep.shutdown();
        }
        // the file this version wrote names the id it was written for
        const fs::path state = b.root / "radio-reports" / "state.json";
        json j = readFailFile(state);
        CHECK(j.is_object() && j.contains("id") && j["id"] == kInstallId);
        if (variant == 0) {   // what 0.99.114 wrote: no id, version 1
            j.erase("id");
            j["v"] = 1;
            std::ofstream f(state, std::ios::binary | std::ios::trunc);
            f << j.dump();
        }
        // wipe the failure files so only state.json can carry the held one
        for (const fs::path& f : b.failFiles()) { std::error_code ec; fs::remove(f, ec); }
        rr::Reporter again;
        rr::Reporter::Options o2 = b.options(true, false, 3);
        o2.failuresPerDay = 1;
        again.configure(o2);                                   // the first start of the new version, Usage reporting ON, the same id
        again.flushAtQuit(300);
        again.shutdown();
        const bool released = b.everything().find("HELDSECRET") != std::string::npos;
        if (variant == 0) {
            CHECK(!released);                                  // cannot be shown to belong to an ON period: dropped
        } else {
            CHECK(released);                                   // written by this version for this id: the 0.99.114 behaviour stands
        }
    }
}

// F3 review 79: turning ON starts from empty - the clearing also runs at the ON edge, and the session starts then
void testOnEdgeStartsFromEmpty() {
    std::printf("F3 the ON edge clears again and starts a fresh session (session start, id and numbering)\n");
    Bench b("f3");
    rr::Reporter rep;
    rep.configure(b.options(true, false, 3));
    const std::string first = rep.sessionId();
    rep.fileFailure(failureOf("00000001"), kRing);
    CHECK(b.failFiles().size() == 1);
    // a file the OFF edge could not delete (a sharing violation then) is gone at the ON edge
    const fs::path keep = b.failFiles().front();
    std::string content;
    { std::ifstream f(keep, std::ios::binary); content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
    std::atomic<bool> fault{true};
    rr::Reporter::setFileFaultHookForTest([&](const char* op, const std::string& path) { return fault.load() && std::string(op) == "remove" && path.find("fail-") != std::string::npos; });
    identityOff(rep);                                          // the delete fails: the file stays, held back from every listing
    CHECK(b.failFiles().size() == 1);
    fault = false;
    rr::Reporter::setFileFaultHookForTest(nullptr);
    b.clock += 600;                                            // ten minutes OFF
    identityOn(rep, kOtherId);
    CHECK(b.failFiles().empty());                              // deleted at the ON edge, not left for a batch to consume
    CHECK(rep.sessionId() != first);
    rr::Event ev;
    ev.kind = "radio_open";
    ev.radio = rr::identify("rtlsdr", "serial=00000001", "RTL-SDR (serial 00000001)");
    ev.word = "start";
    ev.set("phase", "start").set("trigger", "startup-restore");
    rep.event(ev);
    std::int64_t ss = -1;
    std::error_code ec;
    for (fs::directory_iterator it(b.root / "radio-reports", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("journal-", 0) != 0 || it->path().extension() != ".jsonl") { continue; }
        std::ifstream f(it->path(), std::ios::binary);
        std::string line;
        if (std::getline(f, line)) {
            const json h = json::parse(line, nullptr, false);
            if (h.is_object() && h.contains("ss")) { ss = h["ss"].get<std::int64_t>(); }
        }
    }
    CHECK(ss == kT0 + 600);                                    // the session of the ON period began at the ON, not ten minutes earlier
    rep.shutdown();
}

}  // namespace

int main() {
    testOffSendsNothing();
    testFailedStateRewriteAtSwitchOffDeletesTheFile();
    testStateOfAnOlderVersionIsNotReleased();
    testOnEdgeStartsFromEmpty();
    testOnIsUnchanged();
    testQueuedFailuresGoWhenSwitchedOff();
    testPreConfigureBufferWithUsageOff();
    testFirstSessionOnFailure();
    testFilesFromEarlierVersionsAreNeverSent();
    testDegradedResendAfterOptOut();
    testOptOutDuringABatchOfFailures();
    testHeldFailuresDoNotSurviveASwitchOff();
    testOffOnDuringABatchSendsNothingFromBefore();
    testFailureOfAnotherInstallIdIsNeverSent();
    return testSummary("test_radio_reports_off_sends_nothing");
}
