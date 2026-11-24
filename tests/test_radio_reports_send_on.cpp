// THE SEND-ON LANE (0.99.109 SENDS radio failure reports; docs/multi-radio-records/send-on.md). Three things are proved here:
//
//  1. NO TEST, DRIVE OR CI RUN CAN REACH THE REAL SERVICE (core/report_send_guard.hpp). The post observer (crash_upload.hpp) is set to count every destination a
//     code path asks postBounded (and the usage heartbeat's transport) for, and to VETO every one, so this test can prove "no connection to a non-loopback
//     address" without making one. Each way a run is a test or a drive (the ctest variable, the synthetic markers, the diagnostics tree, a redirected profile) is
//     tried against the real transport, the whole reporter with its default transport, and the heartbeat; a real user's run is the positive control (the observer
//     sees telemetry.foxsdr.com and vetoes it).
//  2. WHAT A REAL COPY DOES against what is deployed today and on release day, over real loopback HTTP with the real transport: the Worker of today (`bad id`),
//     the new Worker with no site behind it (502 and 503), the service not there at all, and a service that never answers: kept, backed off 1 min, 5 min, 1 h,
//     6 h, 14 days and then dropped and counted; nothing blocks start-up, tuning or shutdown (the times are printed).
//  3. WHAT IS SENT WITH USAGE REPORTING AND DIAGNOSTICS EACH ON AND OFF, held to the approved text of PRIVACY.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include "core/crash_upload.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/report_send_guard.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;
namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::int64_t kT0 = 1794830400;
const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
const char* const kReal = "https://telemetry.foxsdr.com";

void setEnv(const char* name, const char* value) {   // "" removes it
#if defined(_WIN32)
    _putenv_s(name, value);
    SetEnvironmentVariableA(name, *value ? value : nullptr);
#else
    if (*value) { setenv(name, value, 1); } else { unsetenv(name); }
#endif
}

void clearMarkers() {
    for (const char* v : {"FOXSDR_NO_OUTSIDE_REPORTS", "FOXSDR_SYNTHETIC", "CI", "GITHUB_ACTIONS", "FOXSDR_DIAG_DIR", "FOXSDR_TELEMETRY_URL"}) { setEnv(v, ""); }
}

// every destination any code path asked for, vetoed
struct Observer {
    std::mutex mu;
    std::vector<std::string> urls;
    Observer() {
        setPostObserverForTest([this](const std::string& url) {
            std::lock_guard<std::mutex> lk(mu);
            urls.push_back(url);
            return true;   // veto: nothing is ever connected to
        });
    }
    ~Observer() { setPostObserverForTest(nullptr); }
    int nonLoopback() {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const std::string& u : urls) { if (!rr::isLoopbackUrl(u)) { ++n; } }
        return n;
    }
    int all() { std::lock_guard<std::mutex> lk(mu); return static_cast<int>(urls.size()); }
    void clear() { std::lock_guard<std::mutex> lk(mu); urls.clear(); }
};

struct Temp {
    fs::path p;
    explicit Temp(const char* tag) {
        p = fs::temp_directory_path() / (std::string("rrso-") + tag + "-" + rr::randomHex(8));
        fs::create_directories(p);
    }
    ~Temp() { std::error_code ec; fs::remove_all(p, ec); }
};

rr::FailureInput failure(const char* cls = "busy") {
    rr::FailureInput f;
    f.driverKind = "rtlsdr";
    f.args = "serial=00000001";
    f.nativeLabel = "RTL-SDR (serial 00000001)";
    f.reason = "the radio is in use by another program";
    f.reasonClass = cls;
    f.trigger = rr::Trigger::StartupRestore;
    return f;
}

// The whole reporter with its DEFAULT transport (the real one), the way a shipped interactive run has it.
rr::Reporter::Options realOptions(const Temp& t, const std::string& endpoint, std::atomic<std::int64_t>& clock, bool usageOn) {
    rr::Reporter::Options o;
    o.dir = (t.p / "radio-reports").string();
    o.endpoint = endpoint;
    o.nowEpoch = [&clock]() { return clock.load(); };
    o.liveSend = true;
    if (usageOn) { o.identity.installId = kInstallId; }
    o.identity.version = "0.99.109";
    o.identity.os = "Windows 10.0.22631";
    o.identity.arch = "x64";
    o.identity.launches = 7;
    return o;
}

std::size_t failFiles(const std::string& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("fail-", 0) == 0 && it->path().extension() == ".json") { ++n; }
    }
    return n;
}

// A stand-in service on 127.0.0.1 with canned answers; every request notes the mock clock.
struct Service {
    std::atomic<int> mode{0};   // 0: 204, 1: 400 `bad id`, 2: 503 no site, 3: 502, 4: never answers
    std::atomic<std::int64_t>* clock = nullptr;
    std::mutex mu;
    std::vector<std::int64_t> at;
    std::vector<std::string> bodies;
    std::atomic<bool> release{false};
    httplib::Server srv;
    std::thread th;
    int port = 0;
    explicit Service(std::atomic<std::int64_t>* c) : clock(c) {
        srv.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                at.push_back(clock->load());
                bodies.push_back(req.body);
            }
            switch (mode.load()) {
                case 1: res.status = 400; res.set_content("bad id", "text/plain"); break;
                case 2: res.status = 503; res.set_content("no site configured", "text/plain"); break;
                case 3: res.status = 502; res.set_content("bad gateway", "text/plain"); break;
                case 4:
                    for (int i = 0; i < 1500 && !release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                    res.status = 503;
                    break;
                default: res.status = 204; break;
            }
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
    }
    ~Service() {
        release = true;
        srv.stop();
        if (th.joinable()) { th.join(); }
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/"; }
    std::size_t count() { std::lock_guard<std::mutex> lk(mu); return at.size(); }
    std::vector<std::int64_t> times() { std::lock_guard<std::mutex> lk(mu); return at; }
};

long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
    clearMarkers();
    // the real profile as the operating system names it, for the positive control (this test is not run with a redirected profile)
#if defined(_WIN32)
    const char* realBase = std::getenv("APPDATA");
#else
    const char* realBase = std::getenv("HOME");
#endif
    const std::string realProfile = realBase != nullptr ? std::string(realBase) : std::string();
#if defined(_WIN32)
    const char* const kProfileVar = "APPDATA";
#else
    const char* const kProfileVar = "HOME";
#endif

    // =================================================================================================================================================
    // 1. THE GUARD
    // =================================================================================================================================================
    {
        CHECK(rr::isLoopbackUrl("http://127.0.0.1:9/") && rr::isLoopbackUrl("http://localhost:8080/x") && rr::isLoopbackUrl("http://[::1]:80/"));
        CHECK(!rr::isLoopbackUrl("https://telemetry.foxsdr.com") && !rr::isLoopbackUrl("https://127.0.0.1.evil.example/") &&
              !rr::isLoopbackUrl("https://localhost@evil.example/") && !rr::isLoopbackUrl("") && !rr::isLoopbackUrl("not a url"));
        CHECK(rr::outsideSendRefusal("http://127.0.0.1:9/", "X:/nowhere").empty());   // a stand-in on this computer is always reachable
    }
#if defined(_WIN32)   // (the situations are Windows paths and the Windows system call; the POSIX half is the HOME rule below)
    // THE GUARD MUST NEVER SILENCE A REAL USER (engine merge 11). The decision takes the folders the operating system names as the user's profile, so each situation a real
    // user can be in is given to it exactly; every one must be SENT (empty refusal), and a profile the system does not name must still be refused.
    {
        const std::string kReal = "https://telemetry.foxsdr.com";
        const std::string profile = "C:/Users/pat/AppData/Roaming/foxsdr";
        // 1. the system's profile-folder call FAILED (no names at all): the guard cannot tell, so the real user's report goes out
        CHECK(rr::outsideSendRefusalWithRoots(kReal, profile, {}).empty());
        // 2. the call answered something ODD: empty strings, a relative path, blanks: nothing usable, so the same
        CHECK(rr::outsideSendRefusalWithRoots(kReal, profile, {"", "AppData/Roaming", ".", "   "}).empty());
        //    an odd name beside a good one does not hide the good one, and a profile under none of the usable names is still refused
        CHECK(rr::outsideSendRefusalWithRoots(kReal, profile, {"", "C:/Users/pat/AppData/Roaming"}).empty());
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "D:/scratch/profile/foxsdr", {"", "C:/Users/pat/AppData/Roaming"}) == "redirected");
        // 3. APPDATA redirected by the user's own policy (folder redirection to a share, roaming profile): the system names the share, APPDATA is the same share
        const std::vector<std::string> policy = {"//fileserver/homes/pat/AppData/Roaming", "//fileserver/homes/pat"};
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "//fileserver/homes/pat/AppData/Roaming/foxsdr/radio-reports", policy).empty());
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "//FILESERVER/Homes/Pat/AppData/Roaming/foxsdr", policy).empty());   // another spelling of the same share (case)
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "//fileserver/homes/pat/AppData/Roaming/foxsdr/", policy).empty());   // a trailing slash
        // 4. the profile on ANOTHER DRIVE: the user's own persistent APPDATA (the system's list carries it) or the profile folder moved there
        const std::vector<std::string> otherDrive = {"C:/Users/pat/AppData/Roaming", "D:/Profiles/pat/AppData/Roaming"};
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "D:/Profiles/pat/AppData/Roaming/foxsdr", otherDrive).empty());
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "d:/profiles/PAT/appdata/roaming/foxsdr/radio-reports", otherDrive).empty());
        // and what the guard is for: a test or drive that redirected its profile to a scratch folder and forgot every marker is stopped, and a folder that only
        // STARTS with a real name (a sibling, not a child) is not under it
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "D:/foxsdr-build/drive-x/appdata/foxsdr", otherDrive) == "redirected");
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "C:/Users/pat/AppData/Roaming-evil/foxsdr", otherDrive) == "redirected");
        // an empty queue folder is still nothing to send from
        CHECK(rr::outsideSendRefusalWithRoots(kReal, "", otherDrive) == "redirected");
        // the markers still refuse whatever the roots say
        for (const char* var : {"FOXSDR_NO_OUTSIDE_REPORTS", "FOXSDR_DIAG_DIR"}) {
#if defined(_WIN32)
            ::_putenv_s(var, "1");
#else
            ::setenv(var, "1", 1);
#endif
            CHECK(!rr::outsideSendRefusalWithRoots(kReal, profile, otherDrive).empty());
#if defined(_WIN32)
            ::_putenv_s(var, "");
#else
            ::unsetenv(var);
#endif
        }
        // and the real call on THIS machine, with the real system names: the real profile folder is sent from, a scratch folder is not (the system call answered here)
        if (!realProfile.empty()) {
            CHECK(rr::outsideSendRefusal(kReal, (fs::path(realProfile) / "foxsdr").string()).empty());
        }
    }
#endif
    {
        Temp t("guard");
        const std::string redirected = (t.p / "radio-reports").string();   // a profile that is not the user's own
        Observer obs;
        auto cancel = std::make_shared<UploadCancel>();

        struct Way { const char* name; const char* var; const char* val; const char* want; };
        const Way ways[] = {
            {"ctest", "FOXSDR_NO_OUTSIDE_REPORTS", "1", "env"},
            {"synthetic", "FOXSDR_SYNTHETIC", "1", "synthetic"},
            {"ci", "CI", "true", "synthetic"},
            {"github", "GITHUB_ACTIONS", "true", "synthetic"},
            {"diagdir", "FOXSDR_DIAG_DIR", "D:/x", "diag-dir"},
        };
        for (const Way& w : ways) {
            clearMarkers();
            setEnv(w.var, w.val);
            obs.clear();
            // even with the profile looking REAL, the marker alone stops it
            const std::string why = rr::outsideSendRefusal(kReal, realProfile.empty() ? redirected : (fs::path(realProfile) / "foxsdr" / "radio-reports").string());
            CHECK(why == w.want);
            const rr::Transport tr = rr::makeRealTransport(cancel, redirected);
            const rr::PostResult res = tr(kReal, "{}");
            CHECK(!res.attempted && res.status == 0);
            CHECK(obs.nonLoopback() == 0 && obs.all() == 0);   // the post was never even asked for
            if (obs.all() != 0) { std::printf("  way %s reached the network layer\n", w.name); }
        }
        // a REDIRECTED PROFILE with no marker at all (a drive that forgot every variable)
        clearMarkers();
        {
            obs.clear();
            CHECK(rr::outsideSendRefusal(kReal, redirected) == "redirected");
            const rr::PostResult res = rr::makeRealTransport(cancel, redirected)(kReal, "{}");
            CHECK(!res.attempted && obs.all() == 0);
            CHECK(rr::outsideSendRefusal(kReal, "") == "redirected");   // no folder to prove: nothing leaves
        }
        // THE WHOLE REPORTER, default transport, a hermetic profile, the real endpoint: a failure and a journal event are queued and the batch makes no post
        {
            obs.clear();
            std::atomic<std::int64_t> clock{kT0};
            Temp t2("chain");
            rr::Reporter rep;
            rep.configure(realOptions(t2, kReal, clock, true));
            rep.fileFailure(failure(), {});
            CHECK(failFiles(rep.dir()) == 1);
            CHECK(rep.runBatchNow() == 0);
            CHECK(rep.stats().posts == 1);                      // the sender tried ...
            CHECK(obs.all() == 0 && obs.nonLoopback() == 0);   // ... and the guard stopped it before any socket
            CHECK(failFiles(rep.dir()) == 1);                   // kept, to be sent from a real run
            rep.shutdown();
        }
        // THE USAGE HEARTBEAT's transport, the other route to the same host: no marker, a redirected profile (what a drive does to keep the real config safe)
        {
            obs.clear();
            setEnv(kProfileVar, t.p.string().c_str());
            std::promise<bool> done;
            {
                TelemetryReporter r;
                r.send(kReal, "{\"id\":\"x\"}", [&done](bool ok) { done.set_value(ok); });
                CHECK(done.get_future().get() == false);
            }
            CHECK(obs.all() == 0);
            setEnv(kProfileVar, realProfile.c_str());
        }
        // POSITIVE CONTROL: a real user's run (no marker, the profile is the user's own) DOES ask for the host; the observer vetoes it, so nothing connects
        if (!realProfile.empty()) {
            clearMarkers();
            obs.clear();
            const std::string mine = (fs::path(realProfile) / "foxsdr" / "radio-reports").string();
            CHECK(rr::outsideSendRefusal(kReal, mine).empty());
            const rr::PostResult res = rr::makeRealTransport(cancel, mine)(kReal, "{}");
            CHECK(!res.attempted);   // vetoed by the observer
            CHECK(obs.nonLoopback() == 1);
            {
                std::lock_guard<std::mutex> lk(obs.mu);
                CHECK(!obs.urls.empty() && obs.urls[0] == kReal);
            }
            // and the heartbeat's real transport asks for it too
            obs.clear();
            std::promise<bool> done;
            {
                TelemetryReporter r;
                r.send(kReal, "{\"id\":\"x\"}", [&done](bool ok) { done.set_value(ok); });
                CHECK(done.get_future().get() == false);
            }
            CHECK(obs.nonLoopback() == 1);
        }
        clearMarkers();
    }

    // =================================================================================================================================================
    // 2. WHAT A REAL COPY DOES, over real loopback HTTP with the real transport (a stand-in on 127.0.0.1 is allowed by the guard)
    // =================================================================================================================================================
    // 2a. THE WORKER OF TODAY answers `bad id`: kept and retried, never dropped before 14 days, never a tight loop
    {
        std::atomic<std::int64_t> clock{kT0};
        Service svc(&clock);
        svc.mode = 1;
        Temp t("badid");
        rr::Reporter rep;
        rep.configure(realOptions(t, svc.url(), clock, true));
        rep.fileFailure(failure(), {});
        CHECK(rep.runBatchNow() == 0 && svc.count() == 1);
        for (int i = 0; i < 300; ++i) { rep.runBatchNow(); }   // a tight loop would show here
        CHECK(svc.count() == 1);                                 // 300 more attempts in the same second: not one more request
        const std::int64_t day = 24 * 3600;
        while (clock.load() < kT0 + 13 * day) { clock += 30; rep.runBatchNow(); }
        CHECK(failFiles(rep.dir()) == 1);                        // still kept on day 13
        CHECK(rep.stats().expired == 0 && rep.stats().refused == 0 && rep.stats().sent == 0);
        CHECK(rep.stats().notReady >= 40);
        const std::vector<std::int64_t> ts = svc.times();
        std::printf("`bad id` for 13 days: %zu requests (about one every six hours after the first hour)\n", ts.size());
        CHECK(ts.size() >= 40 && ts.size() <= 70);
        while (clock.load() < kT0 + 14 * day + 7 * 3600) { clock += 30; rep.runBatchNow(); }
        CHECK(failFiles(rep.dir()) == 0 && rep.stats().expired == 1);   // 14 days, then dropped and counted
        rep.shutdown();
    }
    // 2b. THE NEW WORKER WITH NO SITE BEHIND IT: 503 and 502 kept; back-off 1 min, 5 min, 1 h, 6 h; the service coming up delivers
    {
        std::atomic<std::int64_t> clock{kT0};
        Service svc(&clock);
        svc.mode = 2;
        Temp t("nosite");
        rr::Reporter rep;
        rep.configure(realOptions(t, svc.url(), clock, true));
        rep.fileFailure(failure(), {});
        while (clock.load() < kT0 + 14 * 3600) {
            rep.runBatchNow();
            if (svc.count() == 3) { svc.mode = 3; }   // a 502 behaves the same as a 503
            clock += 10;
        }
        const std::vector<std::int64_t> ts = svc.times();
        CHECK(ts.size() >= 6);
        if (ts.size() >= 6) {
            CHECK(ts[1] - ts[0] == 60);
            CHECK(ts[2] - ts[1] == 300);
            CHECK(ts[3] - ts[2] == 3600);
            CHECK(ts[4] - ts[3] == 21600);
            CHECK(ts[5] - ts[4] == 21600);
        }
        CHECK(failFiles(rep.dir()) == 1 && rep.stats().sent == 0 && rep.stats().refused == 0);
        svc.mode = 0;
        clock += 21600;
        CHECK(rep.runBatchNow() == 1 && failFiles(rep.dir()) == 0);   // the service is up: delivered, and the message is gone
        // the same service down for the whole 14 days: dropped and counted
        rep.fileFailure(failure("bind"), {});
        svc.mode = 2;
        const std::int64_t start = clock.load();
        while (clock.load() < start + 14 * 24 * 3600 + 7 * 3600) { clock += 30; rep.runBatchNow(); }
        CHECK(failFiles(rep.dir()) == 0 && rep.stats().expired == 1);
        rep.shutdown();
    }
    // 2c. THE SERVICE IS NOT THERE AT ALL (nothing listens) and 2d. IT TAKES THE CONNECTION AND NEVER ANSWERS: nothing blocks start-up, tuning or shutdown
    {
        int deadPort = 0;
        {
            httplib::Server s;
            deadPort = s.bind_to_any_port("127.0.0.1");
        }   // closed again
        std::atomic<std::int64_t> clock{kT0};
        Temp t("dead");
        rr::Reporter rep;
        auto t0 = std::chrono::steady_clock::now();
        rep.configure(realOptions(t, "http://127.0.0.1:" + std::to_string(deadPort) + "/", clock, true));
        const long long configureMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        rep.fileFailure(failure(), {});
        rep.poll();
        const long long reportMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        rep.flushAtQuit(1500);   // what the engine passes at quit
        rep.shutdown();
        const long long quitMs = msSince(t0);
        std::printf("service not there: configure %lld ms, fileFailure+poll %lld ms, quit (flushAtQuit 1500 + shutdown) %lld ms\n", configureMs, reportMs, quitMs);
        CHECK(configureMs < 500 && reportMs < 250 && quitMs < 2500);
        CHECK(failFiles(rep.dir()) == 1);   // kept for the next start
    }
    {
        std::atomic<std::int64_t> clock{kT0};
        Service svc(&clock);
        svc.mode = 4;   // never answers (15 s)
        Temp t("hang");
        rr::Reporter rep;
        auto t0 = std::chrono::steady_clock::now();
        rep.configure(realOptions(t, svc.url(), clock, true));
        const long long configureMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        rep.fileFailure(failure(), {});
        rep.poll();   // starts the sender on its own thread
        const long long reportMs = msSince(t0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the send is in flight and stuck
        const std::size_t seen = svc.count();
        t0 = std::chrono::steady_clock::now();
        rep.flushAtQuit(1500);
        const long long flushMs = msSince(t0);
        t0 = std::chrono::steady_clock::now();
        rep.shutdown();
        const long long shutMs = msSince(t0);
        std::printf("service never answers (request in flight: %zu): configure %lld ms, fileFailure+poll %lld ms, flushAtQuit(1500) %lld ms, shutdown %lld ms\n", seen, configureMs,
                    reportMs, flushMs, shutMs);
        CHECK(seen == 1);
        CHECK(configureMs < 500 && reportMs < 250);
        CHECK(flushMs < 2500 && shutMs < 1500);   // quit is never held by a stuck send
        CHECK(failFiles(rep.dir()) == 1);
        svc.release = true;
    }

    // =================================================================================================================================================
    // 3. WHAT IS SENT WITH USAGE REPORTING AND DIAGNOSTICS EACH ON AND OFF (PRIVACY.md, "Radio failure reports and the radio journal")
    // =================================================================================================================================================
    for (int usage = 0; usage <= 1; ++usage) {
        for (int diag = 0; diag <= 1; ++diag) {
            std::atomic<std::int64_t> clock{kT0};
            Temp t("case");
            std::mutex mu;
            std::vector<json> sent;
            rr::Reporter::Options o = realOptions(t, "http://127.0.0.1:9/", clock, usage == 1);
            o.identity.diagOn = diag == 1;
            o.identity.launches = 1;   // the install's first session: its record is sent (with Usage reporting on)
            o.transport = [&](const std::string&, const std::string& body) {
                std::lock_guard<std::mutex> lk(mu);
                sent.push_back(json::parse(body));
                return rr::PostResult{true, 204, 0, ""};
            };
            rr::Reporter rep;
            rep.configure(o);
            rep.setDiagOn(diag == 1);
            const std::string tag = std::string("usage ") + (usage ? "ON" : "OFF") + ", diagnostics " + (diag ? "ON" : "OFF");

            // a radio fails to open: ONE report with Usage reporting on, NONE with it off (the owner's decision of 2026-11-24 reversed "always")
            rep.fileFailure(failure(), {"12:01:07.331 warn rtlsdr: open failed"});
            // journal events, the machine description and the first-session record: only with Usage reporting on
            rr::RadioIdentity rid = rr::identify("rtlsdr", "serial=00000001", "RTL-SDR (serial 00000001)");
            rr::Event ev;
            ev.kind = "radio_open";
            ev.radio = rid;
            ev.word = "start";
            ev.set("phase", "start").set("trigger", "startup-restore");
            rep.event(ev);
            rr::Event env;
            env.kind = "session_env";
            env.set("cpu", "Intel Core i7-8700K").set("audioApi", "wasapi").set("cpuThreads", 12).set("ramMb", 32768).set("gpu", "NVIDIA GeForce GTX 1060");
            rep.event(env);
            rep.firstSession("{\"v\":\"0.99.109\",\"os\":\"Windows 10.0.22631\",\"arch\":\"x64\",\"launches\":1}", "on-failure");
            clock += 5;
            if (usage == 1) { CHECK(rep.runBatchNow() >= 1); } else { CHECK(rep.runBatchNow() == 0); }

            std::size_t fails = 0, journal = 0, first = 0;
            for (const json& m : sent) {
                const std::string kind = m.value("kind", std::string());
                if (kind == "radio_fail") {
                    ++fails;
                    CHECK(m["body"]["diagOn"] == (diag == 1));                       // "whether Diagnostics was on" is a field; it changes nothing sent
                    CHECK(m["body"]["serial"] == "00000001" && m["body"]["driver"] == "rtlsdr");
                    CHECK(!m["body"]["logLines"].empty());
                    CHECK(usage == 1);   // a failure report exists only with Usage reporting on
                    CHECK(m["installId"] == kInstallId && m.contains("sessionId") && m.contains("seq") && !m.contains("messageId"));
                } else if (kind == "first_session") {
                    ++first;
                } else {
                    ++journal;
                }
            }
            std::printf("%s: %zu failure report(s), %zu journal/description message(s), %zu first-session\n", tag.c_str(), fails, journal, first);
            CHECK(fails == (usage == 1 ? 1u : 0u));   // sent with the Diagnostics switch on or off, and ONLY with Usage reporting on
            if (usage == 0) {
                CHECK(sent.empty());                 // "with Usage reporting off FoxSDR sends nothing about your radios or your use"
                CHECK(failFiles(rep.dir()) == 0);
                CHECK(journal == 0 && first == 0);   // "Turning Usage reporting off stops the journal, the machine description, the first-session record"
            } else {
                CHECK(journal == 2);                // the open event and the machine description
            }
            rep.shutdown();
        }
    }
    // switching Usage reporting OFF deletes every queued message: the journal and the queued failures alike (owner's decision of 2026-11-24)
    {
        std::atomic<std::int64_t> clock{kT0};
        Temp t("off");
        std::mutex mu;
        std::vector<json> sent;
        rr::Reporter::Options o = realOptions(t, "http://127.0.0.1:9/", clock, true);
        o.transport = [&](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            sent.push_back(json::parse(body));
            return rr::PostResult{true, 204, 0, ""};
        };
        o.liveSend = false;   // queue only
        rr::Reporter rep;
        rep.configure(o);
        rep.fileFailure(failure(), {});
        rr::Event ev;
        ev.kind = "radio_open";
        ev.radio = rr::identify("rtlsdr", "serial=00000001", "RTL-SDR (serial 00000001)");
        ev.word = "start";
        ev.set("phase", "start").set("trigger", "startup-restore");
        rep.event(ev);
        rr::Identity off = rep.identity();
        off.installId.clear();
        rep.setIdentity(off);
        // nothing of the identity stays on disk
        std::string everything;
        for (const auto& e : fs::recursive_directory_iterator(t.p)) {
            if (!e.is_regular_file()) { continue; }
            std::ifstream f(e.path(), std::ios::binary);
            everything.append(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        CHECK(everything.find(kInstallId) == std::string::npos);
        CHECK(failFiles(rep.dir()) == 0);   // the queued failure is gone with the rest
        rep.shutdown();
    }
    // the still-running beat: off without an install id
    {
        HeartbeatSender h;
        h.configure("https://telemetry.foxsdr.com", "", "0.99.109", 300);
        CHECK(!h.configured());
        HeartbeatSender h2;
        h2.configure("https://telemetry.foxsdr.com", kInstallId, "0.99.109", 300);
        CHECK(h2.configured());
    }

    clearMarkers();
    return testSummary("test_radio_reports_send_on");
}
