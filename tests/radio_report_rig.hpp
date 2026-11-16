// radio_report_rig.hpp - a stand-in report server and a temporary queue folder, so a driver test can see the radio reports a driver raises (the radio-reports lane's
// helpers in core/radio_events.hpp). Nothing is sent anywhere: the transport hands the body to a vector. Used by the S5b/S5c driver tests that prove their
// reporting calls (docs/multi-radio-records/second-merge.md).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/radio_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"

namespace rrrig {

namespace rr = ::cascade::core::radioreport;
using nlohmann::json;
namespace fs = std::filesystem;

struct Server {
    std::mutex mu;
    std::vector<std::string> bodies;
    rr::Transport transport() {
        return [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return rr::PostResult{true, 204, 0, ""};
        };
    }
    std::vector<json> all() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<json> out;
        for (const std::string& b : bodies) { out.push_back(json::parse(b)); }
        return out;
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu);
        bodies.clear();
    }
};

struct Rig {
    fs::path dir;
    std::atomic<std::int64_t> clock{1794830400};
    Server server;
    Rig() {
        static std::atomic<std::int64_t> nextStart{1794830400};   // the reporter keeps its per-control bound across rigs: each rig starts two hours after the last
        clock = nextStart.fetch_add(7200);
        std::random_device rd;
        dir = fs::temp_directory_path() / ("rrrig-" + std::to_string(rd()) + "-" + std::to_string(rd()));
        fs::create_directories(dir);
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.nowEpoch = [this]() { return clock.load(); };
        o.transport = server.transport();
        o.liveSend = true;
        o.identity.installId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 3;
        rr::global().configure(o);
    }
    ~Rig() {
        rr::global().shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::vector<json> flush() {
        server.clear();
        rr::global().runBatchNow();
        return server.all();
    }
};

inline std::vector<json> ofKind(const std::vector<json>& all, const char* kind) {
    std::vector<json> out;
    for (const json& m : all) {
        if (m.value("kind", std::string()) == kind) { out.push_back(m); }
    }
    return out;
}

}  // namespace rrrig
