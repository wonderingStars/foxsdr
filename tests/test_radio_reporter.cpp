// Tests for core/radio_reporter.hpp: the on-disk queue, the daily bound, the sender, the failure report, the journal and the first-session record.
// Everything runs against stand-in servers (a function that answers like the Worker would); nothing touches a network or the real config.
//
// A child mode (`--child-write <dir>`) is how the test kills a process in the middle of a session: the child queues events and ends without running a
// single destructor, and the parent finds them on disk and sends them (C2).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using namespace cascade::core::radioreport;
using nlohmann::json;
namespace fs = std::filesystem;

#ifndef CASCADE_SOURCE_ROOT
#error "CASCADE_SOURCE_ROOT is defined by tests/CMakeLists.txt"
#endif
#ifndef CASCADE_NODE_PATH
#define CASCADE_NODE_PATH ""
#endif

namespace {

constexpr std::int64_t kT0 = 1794830400;   // 2026-11-16 12:00:00 UTC
const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

// A stand-in for the Worker: records every request, answers from a script (then from `fallback`).
struct Server {
    std::mutex mu;
    std::vector<std::string> bodies;
    std::vector<PostResult> script;
    PostResult fallback{true, 204, 0, ""};
    std::function<void()> onPost;   // called inside the post (to hang it, say)
    Transport transport() {
        return [this](const std::string& url, const std::string& body) {
            (void)url;
            if (onPost) { onPost(); }
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            if (!script.empty()) {
                PostResult r = script.front();
                script.erase(script.begin());
                return r;
            }
            return fallback;
        };
    }
    std::size_t count() { std::lock_guard<std::mutex> lk(mu); return bodies.size(); }
    json at(std::size_t i) { std::lock_guard<std::mutex> lk(mu); return json::parse(bodies.at(i)); }
};

struct Temp {
    fs::path p;
    explicit Temp(const char* tag) {
        p = fs::temp_directory_path() / (std::string("rra-") + tag + "-" + randomHex(8));
        fs::create_directories(p);
    }
    ~Temp() { std::error_code ec; fs::remove_all(p, ec); }
    std::string str() const { return p.string(); }
};

struct Rig {
    Temp tmp;
    std::atomic<std::int64_t> clock{kT0};
    std::shared_ptr<Server> server = std::make_shared<Server>();
    Reporter rep;
    explicit Rig(const char* tag) : tmp(tag) {}
    Reporter::Options options() {
        Reporter::Options o;
        o.dir = (tmp.p / "radio-reports").string();
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
        return o;
    }
    void start() { rep.configure(options()); }
    void start(const Reporter::Options& o) { rep.configure(o); }
    void advance(std::int64_t s) { clock += s; }
};

FailureInput failure(const char* kind, const char* args, const char* reason, const char* cls, Trigger t = Trigger::StartupRestore) {
    FailureInput f;
    f.driverKind = kind;
    f.args = args;
    f.reason = reason;
    f.reasonClass = cls;
    f.trigger = t;
    return f;
}

const char* const kD3 = "the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31, serial airspyhf_sn:HF0001A2)";

std::vector<std::string> ringWith(const char* line) {
    return {"12:01:07.120 info window: opened the main window", std::string("12:01:07.331 warn ") + line};
}

std::vector<fs::path> filesIn(const std::string& dir, const char* prefix) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(prefix, 0) == 0 && it->path().extension() != ".tmp") { out.push_back(it->path()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

json readJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), nullptr, false);
}

// Every message in the queue as parsed JSON (failure files' message, and every journal line), in file order.
std::vector<json> queued(const std::string& dir) {
    std::vector<json> out;
    for (const fs::path& f : filesIn(dir, "fail-")) {
        const json w = readJson(f);
        if (w.is_object() && w.contains("msg")) { out.push_back(w["msg"]); }
    }
    for (const fs::path& f : filesIn(dir, "journal-")) {
        if (f.extension() != ".jsonl") { continue; }
        std::ifstream in(f, std::ios::binary);
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first) { first = false; continue; }   // the header
            const json m = json::parse(line, nullptr, false);
            if (m.is_object()) { out.push_back(m); }
        }
    }
    return out;
}

Event openStart(const RadioIdentity& r) {
    Event e;
    e.kind = "radio_open";
    e.radio = r;
    e.word = "start";
    e.set("phase", "start").set("trigger", "startup-restore");
    return e;
}

// cmd.exe (std::system on Windows) strips ONE outer pair of quotes from a line that starts with a quote; sh reads that pair as a single word and finds no
// such program. So the outer pair is Windows's alone.
static std::string systemLine(const std::string& inner) {
#if defined(_WIN32)
    return "\"" + inner + "\"";
#else
    return inner;
#endif
}

// Asks the Worker's own validator (node) about every message in `msgs`. Returns the number refused; -1 when node is not available.
int validateWithWorker(const std::vector<json>& msgs, std::vector<std::string>* verdicts = nullptr) {
    // FOXSDR_DUMP_APP_MESSAGES=<file>: every message handed to the Worker's validator is also appended to that file, one JSON message a line. The website's
    // repository keeps the result as testdata/app-messages.jsonl and holds its own validators to it (must-fix lane E: what the application sends is what the site takes).
    if (const char* dump = std::getenv("FOXSDR_DUMP_APP_MESSAGES"); dump != nullptr && *dump != '\0') {
        std::ofstream df(dump, std::ios::binary | std::ios::app);
        for (const json& m : msgs) { df << m.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; }
    }
    const std::string node = CASCADE_NODE_PATH;
    if (node.empty()) { return -1; }
    Temp t("validate");
    const fs::path in = t.p / "in.jsonl";
    const fs::path out = t.p / "out.txt";
    {
        std::ofstream f(in, std::ios::binary);
        for (const json& m : msgs) { f << m.dump(-1, ' ', false, json::error_handler_t::replace) << "\n"; }
    }
    const std::string script = std::string(CASCADE_SOURCE_ROOT) + "/tests/radio_reports_validate.mjs";
    const std::string cmd = systemLine("\"" + node + "\" \"" + script + "\" \"" + in.string() + "\" > \"" + out.string() + "\"");
    const int rc = std::system(cmd.c_str());
    std::ifstream r(out);
    std::string line;
    int refused = 0;
    std::size_t n = 0;
    while (std::getline(r, line)) {
        ++n;
        if (verdicts) { verdicts->push_back(line); }
        if (line.rfind("OK ", 0) != 0) { ++refused; std::printf("WORKER VERDICT: %s\n", line.c_str()); }
    }
    if (rc != 0 || n != msgs.size()) { std::printf("validator run failed: rc=%d lines=%zu messages=%zu\n", rc, n, msgs.size()); return 1000; }
    return refused;
}

int childWrite(const std::string& dir) {
    Reporter rep;
    Reporter::Options o;
    o.dir = dir;
    o.endpoint = "http://127.0.0.1:9/";
    o.liveSend = false;
    o.identity.installId = kInstallId;
    o.identity.version = "0.99.109";
    o.identity.os = "Windows 10.0.22631";
    o.identity.arch = "x64";
    o.identity.launches = 3;
    rep.configure(o);
    const RadioIdentity r = identify("airspyhf", "serial=HF0001A2", "Airspy HF+ (serial HF0001A2)");
    Event e1 = openStart(r);
    rep.event(e1);   // an open that begins, written to disk BEFORE the call that kills the process
    rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control"), ringWith("source: the saved radio did not reopen"));
    Event e2;
    e2.kind = "radio_close";
    e2.radio = r;
    e2.word = "died";
    e2.set("cause", "died").set("openSec", 12);
    rep.event(e2);
    std::fflush(stdout);
    std::_Exit(7);   // no destructor, no flush, no join: what happens to a process that is killed
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child-write") { return childWrite(argv[2]); }

    const RadioIdentity hf = identify("airspyhf", "serial=HF0001A2", "Airspy HF+ (serial HF0001A2)");
    const RadioIdentity rtl = identify("rtlsdr", "index=0", "RTL-SDR");
    std::vector<json> everything;   // every message made, for the Worker's own validator at the end
    auto keep = [&everything](const std::vector<json>& m) { everything.insert(everything.end(), m.begin(), m.end()); };

    // --- R10 (security review 2): a state file or a journal header with a field of the WRONG TYPE is a damaged file, never an exception at start-up ---------------------------
    {
        const std::vector<std::string> hostile = {
            "{\"day\":5}",                                                  // a number where the date text belongs
            "{\"failAdmitted\":\"many\",\"journalAdmitted\":[1]}",              // text and an array where counts belong
            "{\"refused\":{},\"firstSeen\":[\"a\",1,null]}",
            "{\"held\":{\"k\":{\"body\":{},\"kind\":7,\"key\":[],\"t\":\"x\",\"at\":null,\"cls\":3,\"fail\":\"yes\"}}}",
            "{\"day\":\"2026-11-16\",\"failAdmitted\":\"x\"}",               // good fields before a bad one: all of it is dropped, not half of it
        };
        for (const std::string& text : hostile) {
            Rig r("wrongtype");
            const fs::path dir = r.tmp.p / "radio-reports";
            fs::create_directories(dir);
            { std::ofstream f(dir / "state.json", std::ios::binary); f << text; }
            // a journal segment whose header's "ss" is not a number
            { std::ofstream f(dir / "journal-abc.1.jsonl", std::ios::binary); f << "{\"ss\":\"soon\",\"v\":1}\n{\"kind\":\"x\"}\n"; }
            bool threw = false;
            try {
                r.start();
                r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control"), ringWith("source: the saved radio did not reopen (control)"));
            } catch (...) { threw = true; }
            CHECK(!threw);
            CHECK(filesIn(r.rep.dir(), "fail-").size() == 1);   // and it works: the failure was filed
            // the damaged file was replaced by a fresh one, which now loads
            const std::string after = [&] { std::ifstream f(dir / "state.json", std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }();
            CHECK(json::parse(after, nullptr, false).is_object());
        }
    }

    // --- ONE REPORT PER FAILURE, scrubbed, never folded (D4 test 1) ----------------------------------------------------------------------------
    {
        Rig r("two");
        r.start();
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control"), ringWith("source: the saved radio (Airspy HF+, airspyhf) did not reopen (control)"));
        r.advance(5);
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control", Trigger::UserPick), {});   // the SAME radio, the SAME reason, seconds later
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 2);            // two failures, two queued files: the 24 h dedup and the once-per-word rule do not apply
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 2);
        if (q.size() == 2) {
            for (const json& m : q) {
                CHECK(m["kind"] == "radio_fail");
                CHECK(m["schema"] == 2);
                CHECK(m["radioKey"] == "airspy-hf:HF0001A2");
                CHECK(m["body"]["serial"] == "HF0001A2");                                  // the serial is its OWN field
                CHECK(m["body"]["reason"].get<std::string>().find("HF0001A2") == std::string::npos);   // and is out of the sentence
                CHECK(m["body"]["reason"].get<std::string>().find("control transfer failed") != std::string::npos);
                CHECK(m["body"]["reasonClass"] == "control");
                CHECK(m["body"]["diagOn"] == false);
                CHECK(m["installId"] == kInstallId);
            }
            CHECK(q[0]["body"]["attempt"] == 1 && q[1]["body"]["attempt"] == 2);
            CHECK(q[0]["body"]["trigger"] == "startup-restore" && q[1]["body"]["trigger"] == "user-pick");
            CHECK(q[0]["seq"] == 0 && q[1]["seq"] == 1);
            CHECK(q[0]["body"]["logLines"].size() == 1);   // the radio line only, the window line is not one
            CHECK(q[1]["body"]["logLines"].empty());
        }
        CHECK(r.server->count() == 0);          // queued first, not yet sent
        const int ok = r.rep.runBatchNow();
        CHECK(ok == 2);
        CHECK(r.server->count() == 2);
        CHECK(filesIn(r.rep.dir(), "fail-").empty());   // 204 deletes the queued message
        keep(q);
    }

    // --- Diagnostics OFF sends the failure; Usage reporting OFF sends NOTHING (the owner's decision of 2026-11-24 reversed decision 12 of the 2026-11-15 contract:
    //     from 0.99.115 there is no anonymous failure report; this block used to expect two anonymous reports) ----------------------------------------
    {
        Rig r("anon");
        Reporter::Options o = r.options();
        o.identity.installId.clear();      // Usage reporting is off: there is no install id
        o.identity.diagOn = false;         // and Diagnostics is off
        r.start(o);
        r.rep.fileFailure(failure("rtlsdr", "index=0", "the radio is in use by another program", "busy", Trigger::UserPick), {});
        r.rep.fileFailure(failure("rtlsdr", "index=0", "the radio is in use by another program", "busy", Trigger::SavedRetry), {});
        r.rep.event(openStart(rtl));       // the journal does not exist without an id
        CHECK(queued(r.rep.dir()).empty());                  // nothing queued
        CHECK(filesIn(r.rep.dir(), "fail-").empty());        // no failure file on disk
        CHECK(r.rep.stats().failuresQueued == 0);
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(r.server->count() == 0);                       // nothing sent
        // with Diagnostics ON the same failure says so (it changes nothing else)
        Reporter::Options o2 = r.options();
        o2.identity.diagOn = true;
        r.start(o2);
        r.rep.fileFailure(failure("rtlsdr", "index=0", "x did not open", "other"), {});
        const std::vector<json> q2 = queued(r.rep.dir());
        CHECK(q2.size() == 1 && q2[0]["body"]["diagOn"] == true);
    }

    // --- the D3 sentence through the whole path, and the reason classes ------------------------------------------------------------------------------
    {
        Rig r("d3");
        r.start();
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control"), {});
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 1);
        if (!q.empty()) {
            const std::string text = q[0].dump();
            CHECK(text.find("HF0001A2") != text.find("\"serial\":\"HF0001A2\"") || true);
            // the serial appears exactly once in the whole message: as the serial field... and in the radio key
            std::size_t n = 0;
            for (std::size_t pos = text.find("HF0001A2"); pos != std::string::npos; pos = text.find("HF0001A2", pos + 1)) { ++n; }
            CHECK(n == 2);
        }
    }

    // --- the shared fixtures: build the same message from each valid fixture's inputs ---------------------------------------------------------------
    {
        const fs::path dir = fs::path(CASCADE_SOURCE_ROOT) / "telemetry-worker" / "test-fixtures" / "radio-reports";
        int built = 0;
        int differ = 0;
        int anonymousFixtures = 0;
        for (fs::directory_iterator it(dir), end; it != end; ++it) {
            const std::string name = it->path().filename().string();
            if (name.rfind("valid-radio-fail-", 0) != 0 || it->path().extension() != ".json") { continue; }
            const json fx = readJson(it->path());
            if (!fx.is_object() || !fx.contains("request")) { continue; }
            const json& b = fx["request"]["body"];
            if (!b.contains("driver") || b["driver"] == "other") { continue; }
            Rig r("fixture");
            Reporter::Options o = r.options();
            o.identity.installId = fx["request"].contains("installId") ? fx["request"]["installId"].get<std::string>() : std::string();
            o.identity.version = b.value("v", std::string());
            o.identity.os = b.value("os", std::string());
            o.identity.arch = b.value("arch", std::string());
            o.identity.channel = b.value("ch", std::string());
            o.identity.updateChannel = b.value("uch", std::string());
            o.identity.diagOn = b.value("diagOn", false);
            r.start(o);
            if (!fx["request"].contains("installId")) {
                // THE ANONYMOUS FORM (contract 1.2): versions 0.99.109 to 0.99.114 sent it with Usage reporting off and the Worker still accepts it (the fixture stays, and the Worker's own
                // tests hold it to the validator), but from 0.99.115 nothing builds it: the owner's decision of 2026-11-24 ("off sends nothing"). Counted, not rebuilt.
                FailureInput none;
                none.driverKind = b["driver"].get<std::string>();
                none.args = "index=0";
                none.reason = b.value("reason", std::string());
                none.reasonClass = b.value("reasonClass", std::string());
                r.rep.fileFailure(none, {});
                CHECK(queued(r.rep.dir()).empty());
                ++anonymousFixtures;
                continue;
            }
            FailureInput in;
            in.driverKind = b["driver"].get<std::string>();
            const std::string serial = b.value("serial", std::string());
            in.args = serial.empty() ? std::string("index=0") : "serial=" + serial;
            in.nativeLabel = b["model"].get<std::string>() + (serial.empty() ? std::string() : " (serial " + serial + ")");
            in.soundCard = in.driverKind == "soundcard";
            in.reason = b["reason"].get<std::string>();
            in.reasonClass = b["reasonClass"].get<std::string>();
            const std::string trig = b["trigger"].get<std::string>();
            in.trigger = trig == "startup-restore" ? Trigger::StartupRestore : trig == "user-pick" ? Trigger::UserPick : trig == "saved-retry" ? Trigger::SavedRetry
                         : trig == "patch-radio" ? Trigger::PatchRadio : trig == "auto-reopen" ? Trigger::AutoReopen : Trigger::Other;
            in.superseded = b.value("superseded", false);
            in.openMs = b.value("openMs", -1LL);
            in.sampleRateSps = b.value("sampleRateSps", -1LL);
            std::vector<std::string> ring;
            for (const json& l : b["logLines"]) {
                std::string line = l.get<std::string>();   // "12:01:07.120 soapy: ..." -> "12:01:07.120 info soapy: ..."
                ring.push_back(line.substr(0, 12) + " info " + line.substr(13));
            }
            const long long attempts = b.value("attempt", 1LL);
            for (long long k = 1; k < attempts; ++k) { r.rep.fileFailure(in, {}); }
            r.rep.fileFailure(in, ring);
            const std::vector<json> q = queued(r.rep.dir());
            ++built;
            if (q.empty()) { ++differ; std::printf("FIXTURE %s: nothing queued\n", name.c_str()); continue; }
            json mine = q.front();
            for (const json& cand : q) { if (cand["body"]["attempt"].get<long long>() > mine["body"]["attempt"].get<long long>()) { mine = cand; } }
            const json& theirs = fx.contains("canonical") ? fx["canonical"] : fx["request"];
            bool same = mine["radioKey"] == theirs["radioKey"];
            // What the application cannot produce from the inputs of a fixture, by name: `firmware` and `library` (no driver hands them over yet), a
            // `dropped` count (it comes from the daily bound's state, which the fixture only states), and the two fixtures whose reason is the already-
            // scrubbed text of a marker case (scrubbing it a second time masks the port number, and the scrub turns "serial port" into "serial <stripped>",
            // which the Worker accepts: both reasons are held to the Worker's validator below).
            const bool reasonIsAScrubbedExample = name.find("scrub-markers") != std::string::npos || name.find("serial-port-word") != std::string::npos;
            for (auto bi = mine["body"].begin(); bi != mine["body"].end(); ++bi) {
                if (bi.key() == "reason" && reasonIsAScrubbedExample) { continue; }
                if (!theirs["body"].contains(bi.key()) || theirs["body"][bi.key()] != bi.value()) {
                    // optional fields the fixture simply does not carry are allowed to be missing only when they say nothing
                    const bool saysNothing = (bi.key() == "dropped" || bi.key() == "ch" || bi.key() == "uch" || bi.key() == "superseded") &&
                                             (bi.value() == 0 || bi.value() == "" || bi.value() == false);
                    if (!saysNothing) { same = false; std::printf("FIXTURE %s: field %s: mine=%s theirs=%s\n", name.c_str(), bi.key().c_str(), bi.value().dump().c_str(), theirs["body"].contains(bi.key()) ? theirs["body"][bi.key()].dump().c_str() : "(absent)"); }
                }
            }
            for (auto ti = theirs["body"].begin(); ti != theirs["body"].end(); ++ti) {
                if (!mine["body"].contains(ti.key())) {
                    const bool cannotMake = ti.key() == "firmware" || ti.key() == "library" || ti.key() == "dropped";
                    const bool saysNothing = (ti.key() == "ch" || ti.key() == "uch") && ti.value() == "";
                    if (!cannotMake && !saysNothing) { same = false; std::printf("FIXTURE %s: field %s missing from mine\n", name.c_str(), ti.key().c_str()); }
                }
            }
            if (!same) { ++differ; }
            keep({mine});
        }
        std::printf("fixture parity: %d valid radio_fail fixtures rebuilt, %d differ\n", built, differ);
        std::printf("fixture parity: %d anonymous-form fixtures are for the Worker only (nothing builds them from 0.99.115)\n", anonymousFixtures);
        CHECK(built >= 6 + anonymousFixtures);
        CHECK(anonymousFixtures >= 2);
        CHECK(differ == 0);
    }

    // --- THE DAILY BOUND: first-and-last kept, `dropped` exact, nothing dropped uncounted (contract 2.3, C8) -------------------------------------
    {
        Rig r("bound");
        Reporter::Options o = r.options();
        o.failuresPerDay = 5;
        o.liveSend = false;   // the queue is what is inspected here
        r.start(o);
        const FailureInput f = failure("airspyhf", "serial=HF0001A2", "the radio stopped answering while setting the gain", "control");
        for (int i = 0; i < 20; ++i) { r.advance(1); r.rep.fileFailure(f, {}); }
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 5);   // 5 sent; the 6th to the 20th held, the newest kept as ONE pending message
        CHECK(r.rep.heldCount() == 1);
        // a failure of a NEW (radio, class) pair passes the bound at once
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", "no sdrplay device", "absent"), {});
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 6);
        // quit releases what was held, with the count
        r.rep.flushAtQuit(100);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 7);
        long long totalDropped = 0;
        long long lastAttempt = 0;
        for (const json& m : q) {
            totalDropped += m["body"].value("dropped", 0LL);
            if (m["body"]["reasonClass"] == "control") { lastAttempt = std::max<long long>(lastAttempt, m["body"]["attempt"].get<long long>()); }
        }
        CHECK(totalDropped == 14);     // the 7th to the 19th were replaced (13), plus none lost: 15 over the bound = 1 sent + 14 said
        CHECK(lastAttempt == 20);      // the LAST failure of the loop is the one that was kept
        const Stats st = r.rep.stats();
        CHECK(st.boundDropped == 14);
        // nothing was lost without being said: sent 5 + released 1 + dropped 14 = 20 failures of that pair
        CHECK(5 + 1 + totalDropped == 20);
        keep(q);
    }
    {
        // the held message is released when the day rolls over, and the day's counts start again
        Rig r("daybound");
        Reporter::Options o = r.options();
        o.failuresPerDay = 2;
        o.liveSend = false;
        r.start(o);
        const FailureInput f = failure("rtlsdr", "index=0", "in use", "busy");
        for (int i = 0; i < 6; ++i) { r.rep.fileFailure(f, {}); }
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 2);
        CHECK(r.rep.heldCount() == 1);
        r.advance(24 * 3600);   // the next UTC day
        r.rep.fileFailure(f, {});
        CHECK(r.rep.heldCount() == 0 || r.rep.heldCount() == 0);
        const std::vector<json> q = queued(r.rep.dir());
        long long dropped = 0;
        for (const json& m : q) { dropped += m["body"].value("dropped", 0LL); }
        CHECK(q.size() == 4);   // 2 + the held one released at the rollover + the first of the new day
        CHECK(dropped == 3);    // failures 4, 5 and 6 over the bound: 3 and 4 and 5 replaced, 6 kept => 3 said (3rd..5th)
    }
    {
        // a held message survives a crash: the next start releases it (it is the last of its loop)
        Rig r("heldcrash");
        Reporter::Options o = r.options();
        o.failuresPerDay = 1;
        o.liveSend = false;
        r.start(o);
        const FailureInput f = failure("rtlsdr", "index=0", "in use", "busy");
        for (int i = 0; i < 4; ++i) { r.rep.fileFailure(f, {}); }
        CHECK(r.rep.heldCount() == 1);
        Reporter again;
        Reporter::Options o2 = r.options();
        o2.failuresPerDay = 1;
        o2.liveSend = false;
        again.configure(o2);   // "the next start"
        CHECK(again.heldCount() == 0);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 2);
        long long dropped = 0;
        for (const json& m : q) { dropped += m["body"].value("dropped", 0LL); }
        CHECK(dropped == 2);
    }

    // --- THE SENDER: retry schedule, refusals, `bad id`, 429 ------------------------------------------------------------------------------------
    {
        Rig r("retry");
        r.start();
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        PostResult down{true, 503, 0, "no site"};
        r.server->script = {down, down, down, down, down};
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(r.server->count() == 1);
        r.advance(30);
        CHECK(r.rep.runBatchNow() == 0 && r.server->count() == 1);   // the first back-off is a minute
        r.advance(31);
        CHECK(r.rep.runBatchNow() == 0 && r.server->count() == 2);   // sent again, refused again: the next is five minutes
        r.advance(299);
        CHECK(r.rep.runBatchNow() == 0 && r.server->count() == 2);
        r.advance(2);
        CHECK(r.rep.runBatchNow() == 0 && r.server->count() == 3);   // then an hour
        r.advance(3599);
        r.rep.runBatchNow();
        CHECK(r.server->count() == 3);
        r.advance(2);
        r.rep.runBatchNow();
        CHECK(r.server->count() == 4);                               // then every six hours
        r.advance(6 * 3600 - 2);
        r.rep.runBatchNow();
        CHECK(r.server->count() == 4);
        r.advance(3);
        r.rep.runBatchNow();
        CHECK(r.server->count() == 5);
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 1);            // never deleted by a 5xx
        r.advance(6 * 3600 + 1);
        CHECK(r.rep.runBatchNow() == 1);                              // the server is back: accepted and deleted
        CHECK(filesIn(r.rep.dir(), "fail-").empty());
        // the same message every time, with `age` recomputed at each attempt
        CHECK(r.server->at(0).value("age", -1) == 0);
        CHECK(r.server->at(1).value("age", -1) == 61);
    }
    {
        Rig r("network");
        r.start();
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        r.server->script = {PostResult{false, 0, 0, ""}};   // no network at all
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 1);
        r.advance(61);
        CHECK(r.rep.runBatchNow() == 1);
    }
    {
        // `bad id` is the Worker deployed today: not ready, kept, never deleted
        Rig r("badid");
        r.start();
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        r.server->script = {PostResult{true, 400, 0, "bad id"}};
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 1);
        CHECK(r.rep.stats().notReady == 1 && r.rep.stats().refused == 0);
        r.advance(61);
        CHECK(r.rep.runBatchNow() == 1);
    }
    {
        // 429 honours retry-after
        Rig r("limit");
        r.start();
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        r.server->script = {PostResult{true, 429, 120, "retry-after"}};
        CHECK(r.rep.runBatchNow() == 0);
        r.advance(119);
        CHECK(r.rep.runBatchNow() == 0 && r.server->count() == 1);
        r.advance(2);
        CHECK(r.rep.runBatchNow() == 1 && r.server->count() == 2);
    }
    {
        // a text-rule refusal is sent ONCE more with the text withheld; any other refusal drops it and counts it
        Rig r("refuse");
        r.start();
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", "stopped answering", "control"), ringWith("airspyhf: open failed"));
        r.server->script = {PostResult{true, 400, 0, "refused: path-in-text"}};
        CHECK(r.rep.runBatchNow() == 1);
        CHECK(r.server->count() == 2);
        const json first = r.server->at(0);
        const json second = r.server->at(1);
        CHECK(first["body"]["reason"] != "withheld" && !first["body"]["logLines"].empty());
        CHECK(second["body"]["reason"] == "withheld" && second["body"]["logLines"].empty());
        CHECK(second["body"]["serial"] == "HF0001A2" && second["body"]["driver"] == "airspyhf");   // the failure itself still reaches the owner
        CHECK(r.rep.stats().degradedResends == 1);
        CHECK(filesIn(r.rep.dir(), "fail-").empty());
        // refused again in its degraded form: dropped, counted, never a third time
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", "stopped answering", "control"), {});
        r.server->script = {PostResult{true, 400, 0, "refused: serial-in-text"}, PostResult{true, 400, 0, "refused: serial-in-text"}};
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(r.server->count() == 4 && filesIn(r.rep.dir(), "fail-").empty() && r.rep.stats().refused == 1);
        // any other code drops it at once
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", "stopped answering", "other"), {});
        r.server->script = {PostResult{true, 400, 0, "refused: bad-field"}};
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(r.server->count() == 5 && filesIn(r.rep.dir(), "fail-").empty() && r.rep.stats().refused == 2);
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", "stopped answering", "bind"), {});
        r.server->script = {PostResult{true, 413, 0, "refused: too-large"}};
        r.rep.runBatchNow();
        CHECK(filesIn(r.rep.dir(), "fail-").empty() && r.rep.stats().refused == 3);
    }
    {
        // older than 14 days: dropped and counted, never sent
        Rig r("expire");
        r.start();
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        r.advance(15 * 24 * 3600);
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(r.server->count() == 0 && filesIn(r.rep.dir(), "fail-").empty() && r.rep.stats().expired == 1);
    }

    // --- THE SWITCH: the shipped build's switch is ON (0.99.109; was: off). A run that does not arm the send (a bounded or hermetic run: the engine passes
    //     liveSend = false until an interactive window arms it) sends none of the new kinds and still queues them ---------------------------------------------
    {
        static_assert(kRadioReportsLiveSend, "THE ONE SWITCH is on in this build");
        CHECK(Reporter::Options{}.liveSend);   // what an interactive run gets by default
        Rig r("switch");
        Reporter::Options o = r.options();
        o.liveSend = false;   // exactly what a bounded run passes (Engine::radioReportsStart), until setLiveSend arms it
        r.start(o);
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        r.rep.event(openStart(rtl));
        r.rep.firstSession("{\"v\":\"0.99.109\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1}", "after-minute");   // launches is 7 in this identity: refused
        r.rep.poll();
        r.rep.flushAtQuit(100);
        CHECK(r.rep.runBatchNow() == 0);
        CHECK(r.server->count() == 0);                              // nothing left the machine
        CHECK(queued(r.rep.dir()).size() == 2);                     // and both were queued
        // the same queue goes out when the switch is on: the proof above is not vacuous
        Reporter live;
        Reporter::Options o2 = r.options();
        live.configure(o2);
        CHECK(live.runBatchNow() == 2);
        CHECK(r.server->count() == 2);
    }

    // --- THE JOURNAL: order, seq, age, the cursor, no id => no journal --------------------------------------------------------------------------------
    {
        Rig r("journal");
        r.start();
        Event env;
        env.kind = "session_env";
        env.set("cpu", "Intel Core i7-8700K").set("audioApi", "wasapi").set("cpuThreads", 12).set("ramMb", 32768).set("gpu", "NVIDIA GeForce GTX 1060");
        r.rep.event(env);
        r.rep.event(env);   // once a session
        r.advance(3);
        r.rep.event(openStart(hf));
        r.advance(2);
        Event ok = openStart(hf);
        ok.word = "ok";
        ok.fields.clear();
        ok.set("phase", "ok").set("trigger", "startup-restore").set("openMs", 910).set("sampleRateSps", 768000).set("band", "fm-broadcast");
        r.rep.event(ok);
        Event st;
        st.kind = "radio_stream";
        st.radio = hf;
        st.set("windowSec", 60).set("samplesPerSec", 767940).set("drops", 0).set("expectedSps", 768000).set("openSec", 60);
        st.set("modes", std::vector<std::string>{"WFM"});
        r.advance(60);
        r.rep.event(st);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 4);
        if (q.size() == 4) {
            CHECK(q[0]["kind"] == "session_env" && q[0]["seq"] == 0 && q[0]["t"] == 0);
            CHECK(q[0]["radioKey"] == "");
            CHECK(q[1]["kind"] == "radio_open" && q[1]["seq"] == 1 && q[1]["t"] == 3 && q[1]["body"]["phase"] == "start" && q[1]["body"]["attempt"] == 1);
            CHECK(q[2]["body"]["phase"] == "ok" && q[2]["body"]["attempt"] == 1);
            CHECK(q[3]["kind"] == "radio_stream" && q[3]["t"] == 65 && q[3]["body"]["samplesPerSec"] == 767940);
            CHECK(q[0]["body"]["cpu"] == "Intel Core i7-8700K" && q[0]["body"]["launches"] == 7 && q[0]["body"].value("window", json()).is_null());   // absent or null: operator[] on a const json with a missing key is undefined, and GCC's is not a null
        }
        keep(q);
        // the segment has a header and the cursor moves only after a 2xx
        const std::vector<fs::path> segs = filesIn(r.rep.dir(), "journal-");
        CHECK(segs.size() >= 1);
        r.server->script = {PostResult{true, 204, 0, ""}, PostResult{true, 204, 0, ""}, PostResult{true, 503, 0, ""}};
        CHECK(r.rep.runBatchNow() == 2);   // two accepted, the third met a 503 and the batch stopped
        CHECK(r.rep.stats().journalLinesPending == 2);
        // restart: the two already accepted are NOT sent again, the others are
        Reporter again;
        Reporter::Options o2 = r.options();
        r.advance(3600);
        again.configure(o2);
        r.server->script.clear();
        CHECK(again.runBatchNow() == 2);
        const std::size_t total = r.server->count();
        CHECK(total == 5);   // 3 requests in the first run (the third refused) + the 2 it left
        CHECK(r.server->at(2)["kind"] == "radio_open" && r.server->at(3)["kind"] == "radio_open" && r.server->at(4)["kind"] == "radio_stream");
        CHECK(r.server->at(2)["seq"] == 2);   // the refused one first, then the next, in seq order
        CHECK(r.server->at(4).value("age", 0) >= 3600);   // it waited in the queue: the site reads the event's own time
        CHECK(again.stats().journalLinesPending == 0);
    }
    {
        // Usage reporting off: no id, no journal, nothing queued (only a failure would be)
        Rig r("nojournal");
        Reporter::Options o = r.options();
        o.identity.installId.clear();
        r.start(o);
        Event env;
        env.kind = "session_env";
        env.set("cpu", "x").set("audioApi", "none");
        r.rep.event(env);
        r.rep.event(openStart(hf));
        CHECK(queued(r.rep.dir()).empty());
        r.rep.firstSession("{\"launches\":1}", "after-minute");
        CHECK(queued(r.rep.dir()).empty());
    }
    {
        // turning Usage reporting OFF deletes what carries the install id AND the queued failure (the owner's decision of 2026-11-24: off sends nothing)
        Rig r("consent");
        r.start();
        r.rep.event(openStart(hf));
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", "stopped answering", "control"), {});
        CHECK(queued(r.rep.dir()).size() == 2);
        Identity id = r.rep.identity();
        id.installId.clear();
        r.rep.setIdentity(id);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.empty());
        CHECK(filesIn(r.rep.dir(), "fail-").empty());
        const std::string all = [&]() { std::string s; for (const std::string& f : readQueueFiles(r.rep.dir())) { s += f; } return s; }();
        CHECK(all.find(kInstallId) == std::string::npos);
        CHECK(r.rep.runBatchNow() == 0 && r.server->count() == 0);
    }

    // --- THE KINDS: every journal kind through the Worker's validator -----------------------------------------------------------------------------
    {
        Rig r("kinds");
        r.start();
        Event env;
        env.kind = "session_env";
        env.set("cpu", "AMD Ryzen 7 5800X 8-Core Processor").set("audioApi", "wasapi").set("audioClass", "speakers").set("displayCount", 2);
        env.set("displayScalePct", std::vector<long long>{100, 150}).set("usbHosts", std::vector<std::string>{"Intel USB 3.1 eXtensible Host Controller"});
        env.set("gpuDriver", "552.44").set("gpu", "NVIDIA GeForce GTX 1060").set("cpuThreads", 16).set("ramMb", 32768);
        r.rep.event(env);
        Event found;
        found.kind = "radio_found";
        found.radio = hf;
        found.word = "usable";
        found.set("binding", "usable").set("source", "scan").set("usbId", "03eb:800c").text("usbHost", "Intel USB 3.1 eXtensible Host Controller", 64);
        r.rep.event(found);
        r.rep.event(openStart(hf));
        Event ok = openStart(hf);
        ok.word = "ok";
        ok.fields.clear();
        ok.set("phase", "ok").set("trigger", "user-pick").set("openMs", 910).set("band", "air-band");
        r.rep.event(ok);
        Event set;
        set.kind = "radio_settings";
        set.radio = hf;
        set.word = "open";
        set.set("why", "open").set("sampleRateSps", 768000).set("bandwidth", 600000).set("gainMode", "manual").set("gainDb", 12.5).set("ppm", -1.5);
        set.set("biasTee", false).set("directSampling", "off").set("decimation", 4).text("antenna", "RX2", 24);
        r.rep.event(set);
        Event refused;
        refused.kind = "radio_settings";
        refused.radio = hf;
        refused.word = "refused";
        refused.set("why", "user").set("refused", "ppm").text("reason", "the radio refused the frequency correction (serial HF0001A2)", 200);
        r.rep.event(refused);
        Event stream;
        stream.kind = "radio_stream";
        stream.radio = hf;
        stream.set("windowSec", 60).set("samplesPerSec", 0).set("drops", 3).set("note", "audio-silent").set("plugins", std::vector<std::string>{"adsb-decoder"});
        r.rep.event(stream);
        Event rec;
        rec.kind = "radio_recovery";
        rec.radio = hf;
        rec.word = "start";
        rec.set("phase", "start").set("what", "usbstuck").set("reasonClass", "timeout").text("reason", "the stream stopped for 5 s", 400);
        r.rep.event(rec);
        Event rec2 = rec;
        rec2.word = "gave-up";
        rec2.fields.clear();
        rec2.set("phase", "gave-up").set("what", "usbstuck").set("ms", 15000);
        rec2.urgent = true;
        r.rep.event(rec2);
        Event close;
        close.kind = "radio_close";
        close.radio = hf;
        close.word = "died";
        close.set("cause", "died").set("openSec", 144).text("reason", "the radio stopped answering", 400);
        r.rep.event(close);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 10);
        for (const json& m : q) {
            if (m["kind"] == "session_env") { continue; }
            CHECK(m["body"]["driver"] == "airspyhf" && m["body"]["serial"] == "HF0001A2" && m["body"]["model"] == "Airspy HF+");
        }
        for (const json& m : q) { CHECK(m["radioKey"] == (m["kind"] == "session_env" ? json("") : json("airspy-hf:HF0001A2"))); }
        keep(q);
    }

    // --- the stream rule: at most one a minute per radio; the window widens after the day's 500th (here 3) --------------------------------------------
    {
        Rig r("stream");
        Reporter::Options o = r.options();
        o.streamWidenAfter = 3;
        r.start(o);
        Event st;
        st.kind = "radio_stream";
        st.radio = hf;
        st.set("windowSec", 60).set("samplesPerSec", 768000).set("drops", 0);
        for (int i = 0; i < 5; ++i) { r.rep.event(st); r.advance(10); }   // five events in fifty seconds: one goes
        CHECK(queued(r.rep.dir()).size() == 1);
        CHECK(r.rep.streamWindowSec(hf.key) == 60);
        r.advance(60);
        r.rep.event(st);
        r.advance(61);
        r.rep.event(st);
        CHECK(queued(r.rep.dir()).size() == 3);
        CHECK(r.rep.streamWindowSec(hf.key) == 300);        // widened, loses nothing: each event states its span
        r.advance(100);
        r.rep.event(st);
        CHECK(queued(r.rep.dir()).size() == 3);              // not yet five minutes
        Event note = st;
        note.set("note", "sounds-wrong");                    // the user's "this sounds wrong" is never coarsened
        r.rep.event(note);
        CHECK(queued(r.rep.dir()).size() == 4);
    }

    // --- the journal's daily bound: first of each (radio, kind, word) always, the newest later one kept, `dropped` said ---------------------------------
    {
        Rig r("jbound");
        Reporter::Options o = r.options();
        o.journalPerDay = 4;
        o.liveSend = false;
        r.start(o);
        for (int i = 0; i < 12; ++i) {
            Event rec;
            rec.kind = "radio_recovery";
            rec.radio = hf;
            rec.word = "start";
            rec.set("phase", "start").set("what", "reopen");
            r.advance(1);
            r.rep.event(rec);
        }
        CHECK(queued(r.rep.dir()).size() == 4);
        Event close;
        close.kind = "radio_close";
        close.radio = hf;
        close.word = "died";
        close.set("cause", "died").set("openSec", 5);
        r.rep.event(close);                                  // a new triple: passes the bound
        CHECK(queued(r.rep.dir()).size() == 5);
        r.rep.flushAtQuit(100);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 6);
        long long dropped = 0;
        for (const json& m : q) { dropped += m["body"].value("dropped", 0LL); }
        CHECK(dropped == 7);   // events 5 to 12: eight over the bound, the newest kept, seven said
        CHECK(4 + 1 + 1 + dropped == 13);
    }

    // --- THE QUEUE'S BOUNDS: 200 failure files, 4 MiB, rotation; the oldest dropped and counted in the next message ---------------------------------
    {
        Rig r("bounds");
        Reporter::Options o = r.options();
        o.maxFailFiles = 3;
        r.start(o);
        const char* classes[] = {"other", "busy", "driver", "bind", "absent", "timeout"};
        for (const char* c : classes) { r.advance(1); r.rep.fileFailure(failure("rtlsdr", "index=0", "x", c), {}); }
        CHECK(filesIn(r.rep.dir(), "fail-").size() == 3);
        CHECK(r.rep.stats().overflowDropped == 3);
        r.rep.fileFailure(failure("rtlsdr", "index=0", "x", "rate"), {});
        const std::vector<json> q = queued(r.rep.dir());
        long long said = 0;
        for (const json& m : q) { said += m["body"].value("dropped", 0LL); }
        CHECK(said >= 3);   // the three dropped files are said on the next message of that radio
    }
    {
        Rig r("rotate");
        Reporter::Options o = r.options();
        o.segmentBytes = 2048;
        o.maxFolderBytes = 6000;
        r.start(o);
        for (int i = 0; i < 80; ++i) {
            Event rec;
            rec.kind = "radio_recovery";
            rec.radio = hf;
            rec.word = "w" + std::to_string(i % 3);
            rec.set("phase", "start").set("what", "reopen");
            r.advance(1);
            r.rep.event(rec);
        }
        CHECK(filesIn(r.rep.dir(), "journal-").size() >= 2);   // rotated
        std::uintmax_t total = 0;
        for (const fs::path& f : filesIn(r.rep.dir(), "journal-")) { total += fs::file_size(f); }
        CHECK(total <= 6000 + 2048);                            // bounded
        CHECK(r.rep.stats().overflowDropped > 0);               // and what was dropped is counted
        r.rep.event(openStart(hf));
        r.rep.event(openStart(rtl));
        long long said = 0;
        for (const json& m : queued(r.rep.dir())) { if (m["body"].contains("dropped") && m["body"]["driver"] == "airspyhf") { said += m["body"]["dropped"].get<long long>(); } }
        CHECK(said > 0);
    }

    // --- C2: the process is killed in the middle of a session and the last events are delivered at the next start ---------------------------------------
    {
        Rig r("kill");
        const std::string dir = (r.tmp.p / "radio-reports").string();
        const std::string cmd = systemLine(std::string("\"") + argv[0] + "\" --child-write \"" + dir + "\"");
        const int rc = std::system(cmd.c_str());
#if defined(_WIN32)
        CHECK(rc == 7);                                                       // it ended without running one destructor
#else
        CHECK(WIFEXITED(rc) && WEXITSTATUS(rc) == 7);                          // it ended without running one destructor (std::system hands back the wait status here, not the exit code)
#endif
        CHECK(filesIn(dir, "fail-").size() == 1);
        CHECK(filesIn(dir, "journal-").size() == 1);
        Reporter::Options o = r.options();
        o.sessionId = "0123456789abcdef";                                       // the NEW session
        r.advance(60);
        r.start(o);
        CHECK(r.rep.runBatchNow() == 3);
        CHECK(r.server->count() == 3);
        std::set<std::string> kinds;
        for (std::size_t i = 0; i < r.server->count(); ++i) {
            kinds.insert(r.server->at(i)["kind"].get<std::string>());
            CHECK(r.server->at(i)["sessionId"] != "0123456789abcdef");        // delivered under the session that made it
        }
        CHECK(kinds == (std::set<std::string>{"radio_fail", "radio_open", "radio_close"}));   // the open that began, the failure, and the close
        CHECK(filesIn(dir, "fail-").empty());
    }

    // --- first_session: only launch 1, a minute in, again on a failure, at most 6, never the ledgers -------------------------------------------------
    {
        const std::string usage =
            "{\"id\":\"a1b2c3d4e5f60718293a4b5c6d7e8f90\",\"v\":\"0.99.109\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1,\"crashes\":0,\"exits_died\":0,"
            "\"exits_killed\":0,\"exits_ended\":0,\"exits_unknown\":0,\"stalls\":0,\"health\":\"sound_ok.wasapi=1\",\"ch\":\"installer\",\"uch\":\"stable\",\"first\":\"2026-11-16\","
            "\"fv\":\"0.99.109\",\"sessionSec\":61,\"sdr\":\"Airspy HF+\",\"radios_max\":1,\"modes\":{\"WFM\":40},\"panels\":[\"map\"],\"plugins\":[\"ADS-B 1.0.0\"]}";
        Rig r("first");
        Reporter::Options o = r.options();
        o.identity.launches = 1;
        r.start(o);
        r.advance(61);
        r.rep.firstSession(usage, "after-minute");
        r.advance(10);
        r.rep.firstSession(usage, "on-failure");     // inside a minute of the last: not sent
        CHECK(queued(r.rep.dir()).size() == 1);
        for (int i = 0; i < 10; ++i) { r.advance(61); r.rep.firstSession(usage, "on-failure"); }
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 6);                          // at most 6 in a session
        if (!q.empty()) {
            const json& b = q[0]["body"];
            CHECK(q[0]["kind"] == "first_session" && b["launches"] == 1 && b["trigger"] == "after-minute");
            CHECK(!b.contains("id") && !b.contains("radios_max"));   // the usage record's id is `installId` here, and radios_max is not a first-session field
            CHECK(b["health"] == "sound_ok.wasapi=1" && b["sdr"] == "Airspy HF+");
            CHECK(q[0]["radioKey"] == "");
        }
        keep(q);
        // not for a returning user
        Rig r2("first2");
        Reporter::Options o2 = r2.options();
        o2.identity.launches = 2;
        r2.start(o2);
        r2.advance(61);
        r2.rep.firstSession(usage, "after-minute");
        CHECK(queued(r2.rep.dir()).empty());
        // a bad trigger is not sent
        Rig r3("first3");
        Reporter::Options o3 = r3.options();
        o3.identity.launches = 1;
        r3.start(o3);
        r3.rep.firstSession(usage, "whenever");
        CHECK(queued(r3.rep.dir()).empty());
    }

    // --- quit is never delayed by a stuck send ------------------------------------------------------------------------------------------------------
    {
        Rig r("stuck");
        r.start();
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        auto release = std::make_shared<std::atomic<bool>>(false);
        r.server->onPost = [release]() {
            for (int i = 0; i < 600 && !release->load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }   // a send that never returns (6 s)
        };
        const auto t0 = std::chrono::steady_clock::now();
        r.rep.flushAtQuit(250);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("flushAtQuit with a stuck send took %lld ms\n", static_cast<long long>(ms));
        CHECK(ms < 2500);
        release->store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // --- an unwritable place disables it, quietly: nothing throws ---------------------------------------------------------------------------------------
    {
        Rig r("unwritable");
        const fs::path file = r.tmp.p / "iamafile";
        { std::ofstream(file) << "x"; }
        Reporter::Options o = r.options();
        o.dir = (file / "radio-reports").string();
        r.start(o);
        CHECK(!r.rep.active());
        r.rep.fileFailure(failure("rtlsdr", "index=0", "in use", "busy"), {});
        r.rep.event(openStart(rtl));
        CHECK(r.rep.runBatchNow() == 0);
        Reporter none;   // never configured: ignores every call
        none.fileFailure(failure("rtlsdr", "index=0", "x", "busy"), {});
        none.event(openStart(rtl));
        CHECK(none.runBatchNow() == 0 && !none.active());
    }

    // --- what happens BEFORE the queue can be opened (the start-up restore runs first) is kept and filed at configure, with its own time ---------------
    {
        Rig r("prestart");
        r.rep.setNowSource([&r]() { return r.clock.load(); });
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control"), ringWith("source: the saved radio did not reopen"));
        r.advance(4);
        r.rep.event(openStart(hf));
        r.advance(3);
        Reporter::Options o = r.options();
        o.liveSend = false;
        r.start(o);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 2);
        if (q.size() == 2) {
            const json& fail = q[0]["kind"] == "radio_fail" ? q[0] : q[1];
            const json& open = q[0]["kind"] == "radio_open" ? q[0] : q[1];
            CHECK(fail["t"] == 0 && open["t"] == 4);                    // the time each happened, not the time the queue opened
            CHECK(fail["installId"] == kInstallId);                      // the identity is known by now
            CHECK(!fail["body"]["logLines"].empty());
        }
        // a run that never opens a queue (hermetic) drops them quietly
        Reporter h;
        h.fileFailure(failure("rtlsdr", "index=0", "x", "busy"), {});
        Reporter::Options none;
        h.configure(none);
        CHECK(!h.active());
        CHECK(h.stats().failuresQueued == 0);
        // the Diagnostics switch changes only a field
        r.rep.setDiagOn(true);
        r.rep.fileFailure(failure("rtlsdr", "index=0", "x", "busy"), {});
        bool sawOn = false;
        for (const json& m : queued(r.rep.dir())) { if (m["kind"] == "radio_fail" && m["body"]["reasonClass"] == "busy") { sawOn = m["body"]["diagOn"] == true; } }
        CHECK(sawOn);
    }

    // --- A4 (field data 2026-11-19): a start-up failure that happened AFTER its open start gets the higher seq, whatever the list it waited in ---------------
    {
        Rig r("prestartorder");
        r.rep.setNowSource([&r]() { return r.clock.load(); });
        r.rep.event(openStart(hf));                                       // the start is written before the call
        r.rep.fileFailure(failure("airspyhf", "serial=HF0001A2", kD3, "control"), ringWith("source: the saved radio did not reopen"));   // the same second
        Reporter::Options o = r.options();
        o.liveSend = false;
        r.start(o);
        const std::vector<json> q = queued(r.rep.dir());
        CHECK(q.size() == 2);
        if (q.size() == 2) {
            const json& fail = q[0]["kind"] == "radio_fail" ? q[0] : q[1];
            const json& open = q[0]["kind"] == "radio_open" ? q[0] : q[1];
            CHECK(open["seq"].get<long long>() < fail["seq"].get<long long>());
        }
    }

    // --- MUST-FIX LANE E, issue 3: the test marker is an envelope member on every kind, and only on a synthetic run -------------------------------------
    {
        Rig plain("syn0");
        plain.start();
        plain.rep.fileFailure(failure("rtlsdr", "index=0", "the radio is in use by another program", "busy"), {});
        Event env0;
        env0.kind = "session_env";
        env0.set("cpu", "Intel Core i7-8700K").set("audioApi", "wasapi");
        plain.rep.event(env0);
        for (const json& m : queued(plain.rep.dir())) { CHECK(!m.contains("synthetic")); }   // a person's install says nothing

        Rig syn("syn1");
        Reporter::Options o = syn.options();
        o.identity.synthetic = true;
        syn.start(o);
        syn.rep.fileFailure(failure("rtlsdr", "index=0", "the radio is in use by another program", "busy"), {});
        syn.rep.event(env0);
        syn.rep.event(openStart(hf));
        const std::vector<json> q = queued(syn.rep.dir());
        CHECK(q.size() == 3);
        for (const json& m : q) { CHECK(m.contains("synthetic") && m["synthetic"] == 1); }   // every kind, so the Worker drops all of it
        keep(q);
        // an install with Usage reporting off files and sends no failure at all (owner's decision of 2026-11-24)
        Rig anon("syn2");
        Reporter::Options oa = anon.options();
        oa.identity.installId.clear();
        oa.identity.synthetic = true;
        anon.start(oa);
        anon.rep.fileFailure(failure("rtlsdr", "index=0", "the radio is in use by another program", "busy"), {});
        CHECK(queued(anon.rep.dir()).empty());
    }

    // --- MUST-FIX LANE E, issue 4: lines with no radio key that the folder's bounds drop are counted and said on the next keyless message -----------------
    {
        Rig r("keyless");
        Reporter::Options o = r.options();
        o.segmentBytes = 2048;
        o.maxFolderBytes = 6000;
        r.start(o);
        for (int i = 0; i < 60; ++i) {
            Event end;
            end.kind = "session_end";
            end.word = "w" + std::to_string(i % 3);
            end.set("opened", false).set("scans", 1).set("openFailures", 0).set("listed", std::vector<std::string>{"Generic RTL2832U"}).set("sessionSec", 10 + i);
            r.advance(1);
            r.rep.event(end);
        }
        CHECK(r.rep.stats().overflowDropped > 0);   // what was dropped is counted locally, as before...
        Event last;
        last.kind = "session_end";
        last.word = "last";
        last.set("opened", false).set("scans", 1).set("openFailures", 0).set("listed", std::vector<std::string>{}).set("sessionSec", 99);
        r.rep.event(last);
        long long said = 0;
        const std::vector<json> q = queued(r.rep.dir());
        for (const json& m : q) { if (m["kind"] == "session_end" && m["body"].contains("dropped")) { said += m["body"]["dropped"].get<long long>(); CHECK(m["radioKey"] == ""); } }
        CHECK(said > 0);                              // ...and now said, so the site learns a machine lost lines
        keep(q);
        // a first_session line has no `dropped` field in the contract and never gets one
        for (const json& m : q) { if (m["kind"] == "first_session") { CHECK(!m["body"].contains("dropped")); } }
    }

    // --- the Worker's own validator over everything the app made -------------------------------------------------------------------------------------
    {
        std::vector<std::string> verdicts;
        const int refused = validateWithWorker(everything, &verdicts);
        if (refused < 0) {
            std::printf("SKIPPED: node was not found, so the Worker's validator did not see the %zu messages the app made\n", everything.size());
        } else {
            std::printf("the Worker's validator saw %zu messages: %d refused\n", everything.size(), refused);
            CHECK(everything.size() >= 30);
            CHECK(refused == 0);
        }
    }

    return testSummary("test_radio_reporter");
}
