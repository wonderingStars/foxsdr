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

    // --- THE LIVE SEND IS OFF: a real beat from the real sender carries no `radios` ------------------------------------------------------------------
    {
        std::mutex mu;
        std::vector<std::string> bodies;
        httplib::Server srv;
        srv.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(req.body);
            res.status = 204;
        });
        const int port = srv.bind_to_any_port("127.0.0.1");
        CHECK(port > 0);
        std::thread th([&] { srv.listen_after_bind(); });
        {
            HeartbeatSender h;
            h.configure("http://127.0.0.1:" + std::to_string(port) + "/", id, "0.99.109", 300);
            CHECK(h.configured());
            h.setRadios({"airspy-hf:HF0001A2"});
            h.poll(0.0);
            for (int i = 0; i < 100; ++i) {
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (!bodies.empty()) { break; }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            h.poll(1000.0);   // joins the finished beat
        }
        srv.stop();
        th.join();
        std::lock_guard<std::mutex> lk(mu);
        CHECK(bodies.size() == 1);
        if (!bodies.empty()) {
            const json beat = json::parse(bodies[0]);
            CHECK(beat["id"] == id && beat["beat"] == 1);
            // THE ONE SWITCH: with kRadioReportsLiveSend false the beat names no radio (the Worker that reads the field is not deployed)
            CHECK(radioreport::kRadioReportsLiveSend == beat.contains("radios"));
        }
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

    return testSummary("test_radio_reports_fields");
}
