// The two fields the radio reports add to messages that already exist (contract 3.10 and 3.11), and the marker that keeps tests out of the user figures:
//   - the running-now beat's `radios` (the keys of the radios open now, at most eight, omitted when none), held back while the live send is off,
//   - the usage record's `window` (`new` or `legacy`, nothing else, left out when unsaid),
//   - `synthetic: 1` on a record from a test, a CI job or the owner's own machine (L11b), and only then.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/session_env.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using nlohmann::json;

namespace {

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) { ::setenv(name, value, 1); } else { ::unsetenv(name); }
#endif
}

}  // namespace

int main() {
    const std::string id = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

    // --- the beat -------------------------------------------------------------------------------------------------------------------------------
    {
        // the two-argument form is today's beat, byte for byte
        const json plain = json::parse(HeartbeatSender::beatJson(id, "0.99.109"));
        CHECK(plain.size() == 3 && plain["id"] == id && plain["v"] == "0.99.109" && plain["beat"] == 1);
        CHECK(HeartbeatSender::beatJson(id, "0.99.109", {}, false) == HeartbeatSender::beatJson(id, "0.99.109"));
        // with radios: the keys, in order, at most eight, nothing else
        const json two = json::parse(HeartbeatSender::beatJson(id, "0.99.109", {"airspy-hf:HF0001A2", "rtl-sdr:-"}, false));
        CHECK(two["radios"].size() == 2 && two["radios"][0] == "airspy-hf:HF0001A2" && two["radios"][1] == "rtl-sdr:-");
        CHECK(two.size() == 4);
        std::vector<std::string> many;
        for (int i = 0; i < 12; ++i) { many.push_back("radio-" + std::to_string(i) + ":-"); }
        CHECK(json::parse(HeartbeatSender::beatJson(id, "0.99.109", many, false))["radios"].size() == 8);
        // the synthetic marker rides the beat too
        CHECK(json::parse(HeartbeatSender::beatJson(id, "0.99.109", {}, true))["synthetic"] == 1);
        // no key carries a frequency, a model text or a serial field: a key is a slug and a serial
        for (const std::string& k : json::parse(HeartbeatSender::beatJson(id, "0.99.109", {"uhd-b200:EDR04ZDB2"}, false))["radios"].get<std::vector<std::string>>()) {
            CHECK(k == radioreport::radioKey("uhd b200", "EDR04ZDB2"));
        }
    }

    // --- THE LIVE SEND IS OFF: the beat the real sender would post carries no `radios` -------------------------------------------------------------
    {
        // (The sender posts over https only, so what it would post is read through nextBeatJson(), which poll() uses and nothing else.)
        HeartbeatSender h;
        h.configure("https://telemetry.foxsdr.com", id, "0.99.109", 300);
        CHECK(h.configured());
        h.setRadios({"airspy-hf:HF0001A2", "rtl-sdr:-"});
        const json beat = json::parse(h.nextBeatJson());
        CHECK(beat["id"] == id && beat["beat"] == 1 && beat["v"] == "0.99.109");
        // THE ONE SWITCH: with kRadioReportsLiveSend false the beat names no radio (the Worker that reads the field is not deployed)
        CHECK(radioreport::kRadioReportsLiveSend == beat.contains("radios"));
        CHECK(!radioreport::kRadioReportsLiveSend ? beat.size() == 3 : beat["radios"].size() == 2);
        // the same sender with the switch on would say them, in the contract's shape: the explicit form is what poll() uses when it is
        const json would = json::parse(HeartbeatSender::beatJson(id, "0.99.109", {"airspy-hf:HF0001A2", "rtl-sdr:-"}));
        CHECK(would["radios"].size() == 2);
    }

    // --- the usage record's `window` ------------------------------------------------------------------------------------------------------------
    {
        TelemetryReport r;
        r.installId = id;
        r.appVersion = "0.99.109";
        CHECK(!json::parse(r.toJson()).contains("window"));          // unsaid: left out
        r.window = "new";
        CHECK(json::parse(r.toJson())["window"] == "new");
        r.window = "legacy";
        CHECK(json::parse(r.toJson())["window"] == "legacy");
        r.window = "C:\\Users\\alice\\window";                         // anything else is never sent
        CHECK(!json::parse(r.toJson()).contains("window"));
        r.window = "NEW";
        CHECK(!json::parse(r.toJson()).contains("window"));
    }

    // --- the test marker (L11b) ----------------------------------------------------------------------------------------------------------------
    {
        const char* const keep[] = {"FOXSDR_SYNTHETIC", "CI", "GITHUB_ACTIONS"};
        std::vector<std::string> saved;
        for (const char* k : keep) { const char* v = std::getenv(k); saved.push_back(v != nullptr ? v : std::string("\x01")); setEnv(k, nullptr); }
        CHECK(!syntheticRun());
        TelemetryReport r;
        r.installId = id;
        r.synthetic = syntheticRun();
        CHECK(!json::parse(r.toJson()).contains("synthetic"));        // a user's record has no marker at all
        setEnv("FOXSDR_SYNTHETIC", "1");
        CHECK(syntheticRun());
        setEnv("FOXSDR_SYNTHETIC", "0");
        CHECK(!syntheticRun());
        setEnv("FOXSDR_SYNTHETIC", nullptr);
        setEnv("CI", "true");
        CHECK(syntheticRun());
        setEnv("CI", nullptr);
        setEnv("GITHUB_ACTIONS", "true");
        CHECK(syntheticRun());
        r.synthetic = true;
        CHECK(json::parse(r.toJson())["synthetic"] == 1);
        for (std::size_t i = 0; i < 3; ++i) { setEnv(keep[i], saved[i] == "\x01" ? nullptr : saved[i].c_str()); }
    }

    // --- THE REAL TRANSPORT, against a real server on loopback (not a stand-in function): the status, the body and the Retry-After header are read -----
    {
        namespace rr = radioreport;
        namespace fs = std::filesystem;
        std::mutex mu;
        std::vector<std::string> bodies;
        std::atomic<int> mode{0};   // 0: 204, 1: 400 bad id, 2: 429 retry-after 90, 3: 400 refused path-in-text, 4: 503
        httplib::Server srv;
        srv.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                bodies.push_back(req.body);
            }
            switch (mode.load()) {
                case 1: res.status = 400; res.set_content("bad id", "text/plain"); break;
                case 2: res.status = 429; res.set_header("Retry-After", "90"); res.set_content("retry-after", "text/plain"); break;
                case 3: res.status = 400; res.set_content("refused: path-in-text", "text/plain"); break;
                case 4: res.status = 503; res.set_content("no site configured", "text/plain"); break;
                default: res.status = 204; break;
            }
        });
        const int port = srv.bind_to_any_port("127.0.0.1");
        CHECK(port > 0);
        std::thread th([&] { srv.listen_after_bind(); });
        const fs::path dir = fs::temp_directory_path() / ("rrf-" + rr::randomHex(8));
        fs::create_directories(dir);
        std::atomic<std::int64_t> clock{1794830400};
        rr::Reporter rep;
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:" + std::to_string(port) + "/";
        o.liveSend = true;     // the default transport: core::postBounded
        o.nowEpoch = [&clock]() { return clock.load(); };
        o.identity.installId = id;
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        rep.configure(o);
        auto file = [&rep](const char* cls) {
            rr::FailureInput f;
            f.driverKind = "rtlsdr";
            f.args = "serial=00000001";
            f.nativeLabel = "RTL-SDR (serial 00000001)";
            f.reason = "the radio is in use by another program";
            f.reasonClass = cls;
            rep.fileFailure(f, {});
        };
        auto queuedFiles = [&dir]() {
            int n = 0;
            for (const auto& e : fs::directory_iterator(dir / "radio-reports")) { if (e.path().filename().string().rfind("fail-", 0) == 0 && e.path().extension() == ".json") { ++n; } }
            return n;
        };
        file("busy");
        mode = 1;   // the Worker deployed today: `bad id`
        CHECK(rep.runBatchNow() == 0);
        CHECK(queuedFiles() == 1 && rep.stats().notReady == 1);
        clock += 61;
        mode = 2;   // a 429 with Retry-After: 90
        CHECK(rep.runBatchNow() == 0);
        CHECK(queuedFiles() == 1);
        clock += 60;
        const std::size_t before = bodies.size();
        CHECK(rep.runBatchNow() == 0);      // 60 s into the 90: nothing is sent
        CHECK(bodies.size() == before);
        clock += 31;
        mode = 4;   // the Worker has no site: 503, kept
        CHECK(rep.runBatchNow() == 0 && queuedFiles() == 1);
        clock += 400;   // the 503 backs off five minutes
        mode = 3;   // a text-rule refusal: resent once with the text withheld, which is refused again, so dropped and counted
        CHECK(rep.runBatchNow() == 0);
        CHECK(queuedFiles() == 0 && rep.stats().refused == 1 && rep.stats().degradedResends == 1);
        file("bind");
        mode = 0;
        CHECK(rep.runBatchNow() == 1);
        CHECK(queuedFiles() == 0 && rep.stats().sent == 1);
        {
            std::lock_guard<std::mutex> lk(mu);
            CHECK(json::parse(bodies.back())["kind"] == "radio_fail" && json::parse(bodies.back())["body"]["reasonClass"] == "bind");
            // the degraded resend said what it withheld
            bool sawWithheld = false;
            for (const std::string& b : bodies) { if (json::parse(b)["body"]["reason"] == "withheld") { sawWithheld = true; } }
            CHECK(sawWithheld);
        }
        rep.shutdown();
        srv.stop();
        th.join();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- THE MACHINE, read once a session (core/session_env): what this computer says, held to the contract's text rules ---------------------------------
    {
        const SessionEnv e = readSessionEnv();
        std::printf("session_env: cpu %zu chars, %d threads, %llu MB, gpu %zu chars, driver %zu chars, %zu usb host(s), %d display(s), %zu scale(s)\n", e.cpu.size(), e.cpuThreads,
                    static_cast<unsigned long long>(e.ramMb), e.gpu.size(), e.gpuDriver.size(), e.usbHosts.size(), e.displayCount, e.displayScalePct.size());
        CHECK(e.cpuThreads > 0);
        CHECK(e.usbHosts.size() <= 8 && e.displayScalePct.size() <= 8);
#if defined(_WIN32)
        CHECK(!e.cpu.empty() && e.ramMb > 0);
#endif
        // whatever the machine said, as the application sends it: scrubbed text passes the Worker's rules, and a processor's clock speed never rides in
        CHECK(e.cpu.find('@') == std::string::npos);
        for (const std::string& t : {radioreport::scrubText(e.cpu, std::string(), 64), radioreport::scrubText(e.gpu, std::string(), 64)}) {
            CHECK(radioreport::textCode(t, std::string()).empty());
        }
        for (const std::string& h : e.usbHosts) { CHECK(radioreport::textCode(radioreport::scrubText(h, std::string(), 64), std::string()).empty()); }
        for (long long pct : e.displayScalePct) { CHECK(pct >= 25 && pct <= 1000); }
    }

    // --- THE START-UP COST: the queue is opened on the start-up path, before the first window ------------------------------------------------------------
    {
        namespace rr = radioreport;
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / ("rrc-" + rr::randomHex(8));
        fs::create_directories(dir);
        rr::Reporter first;
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.identity.installId = id;
        o.identity.version = "0.99.109";
        const auto t0 = std::chrono::steady_clock::now();
        first.configure(o);   // the first start: the folder and the state file are made
        const double firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        first.shutdown();
        rr::Reporter again;
        const auto t1 = std::chrono::steady_clock::now();
        again.configure(o);   // every start after: nothing to say, nothing written
        const double againMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        std::printf("radio reports: opening the queue took %.1f ms the first time and %.1f ms after\n", firstMs, againMs);
        CHECK(firstMs < 250.0);
        CHECK(againMs < 250.0);
        again.shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    return testSummary("test_radio_reports_fields");
}
