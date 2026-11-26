// test_off_no_heartbeat.cpp - USAGE REPORTING OFF SENDS NOTHING EVEN WHEN THE SETTINGS FILE STILL HOLDS AN INSTALL ID (brief H1, 2026-11-26).
//
// The switch-off click clears the install id, but a settings file can say "telemetryEnabled": false AND carry a valid id (a hand edit, a restored or copied file, a crash between two writes).
// A real EngineService (the real Engine, heartbeat armed as the window arms it) is started on such a file with every send address pointed at a stand-in on 127.0.0.1 that only counts the
// connections it is handed. Over a run that would normally beat (a radio that will not open is reported as well) the count on every stand-in must stay ZERO, the engine must hold no id, and
// the saved file must not carry one. The control (switch ON with the same id) must connect to the telemetry stand-in, so a zero is not a dead stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/radio_events.hpp"
#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using namespace cascade::core;
using cascade::engine::EngineService;
using cascade::engine::ServiceLock;
using cascade::engine::ServiceOptions;

namespace {

#if defined(_WIN32)
using SockT = SOCKET;
constexpr SockT kBadSock = INVALID_SOCKET;
void closeSock(SockT s) { ::closesocket(s); }
#else
using SockT = int;
constexpr SockT kBadSock = -1;
void closeSock(SockT s) { ::close(s); }
#endif

// Accepts and immediately closes; counts. Binds 127.0.0.1 only.
class CountingListener {
public:
    CountingListener() {
#if defined(_WIN32)
        WSADATA wd;
        ::WSAStartup(MAKEWORD(2, 2), &wd);
#endif
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (listen_ == kBadSock || ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen_, 16) != 0) { return; }
        socklen_t len = sizeof(a);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(listen_, &fds);
                timeval tv{0, 50 * 1000};
                if (::select(static_cast<int>(listen_) + 1, &fds, nullptr, nullptr, &tv) > 0) {
                    const SockT c = ::accept(listen_, nullptr, nullptr);
                    if (c != kBadSock) {
                        ++count_;
                        closeSock(c);
                    }
                }
            }
        });
    }
    ~CountingListener() {
        stop_.store(true);
        if (thread_.joinable()) { thread_.join(); }
        if (listen_ != kBadSock) { closeSock(listen_); }
    }
    bool ok() const { return port_ != 0; }
    int count() const { return count_.load(); }
    std::string https() const { return "https://127.0.0.1:" + std::to_string(port_) + "/x"; }

private:
    SockT listen_ = kBadSock;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> count_{0};
    std::thread thread_;
};

struct Address {
    const char* name;
    const char* env;
};
const Address kAddresses[] = {
    {"telemetry", "FOXSDR_TELEMETRY_URL"}, {"crash", "FOXSDR_CRASH_URL"},     {"update", "FOXSDR_UPDATE_URL"},
    {"feature", "FOXSDR_FEATURE_URL"},     {"problem", "FOXSDR_PROBLEM_URL"}, {"catalogue", "FOXSDR_REGIONAL_CATALOGUE_URL"},
    {"beta", "FOXSDR_BETA_API_URL"},       {"tester", "FOXSDR_TESTER_USAGE_URL"},
};
constexpr int kN = static_cast<int>(sizeof(kAddresses) / sizeof(kAddresses[0]));
constexpr const char* kId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

std::vector<std::unique_ptr<CountingListener>> g_stand;

int total() {
    int n = 0;
    for (const auto& s : g_stand) { n += s->count(); }
    return n;
}
std::string summary() {
    std::string out;
    for (int i = 0; i < kN; ++i) {
        if (g_stand[static_cast<std::size_t>(i)]->count() > 0) { out += std::string(out.empty() ? "" : ",") + kAddresses[i].name + "=" + std::to_string(g_stand[static_cast<std::size_t>(i)]->count()); }
    }
    return out.empty() ? std::string("-") : out;
}

void tick(EngineService& svc, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        svc.keep();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void seed(const fs::path& cfg, bool usage) {
    fs::create_directories(cfg.parent_path());
    AppConfig s;
    s.volume = 0.0f;
    s.telemetryEnabled = usage;
    s.telemetryInstallId = kId;
    s.telemetryFirstRun = "2026-01-02";
    s.telemetryFirstVersion = "0.99.80";
    s.telemetryLaunches = 3;
    s.telemetrySessionStarted = 1790000000;
    s.telemetryCleanExit = true;
    s.diagnosticsEnabled = false;
    s.updateCheckEnabled = false;
    std::ofstream out(cfg, std::ios::binary | std::ios::trunc);
    out << ConfigStore::serialize(s);
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Outcome {
    int connections = 0;
    int telemetryConnections = 0;
    std::string seen;
    bool heartbeatConfigured = false;
    bool savedHasId = false;
};

// One engine run, heartbeat armed as the window arms it, a radio that will not open reported, long enough for the first beat.
Outcome runOnce(const fs::path& dir, bool usage) {
    const fs::path cfg = dir / "k" / "config.json";
    seed(cfg, usage);
    const int before = total();
    const int beforeTel = g_stand[0]->count();
    Outcome o;
    ServiceOptions so;
    so.configPath = cfg.string();
    so.diagnosticsDir = (dir / "crashes").string();
    so.lock = ServiceLock::BestEffort;
    so.heartbeat = true;
    {
        EngineService svc(so);
        std::string sentence;
        CHECK(svc.acquireConfig(sentence));
        const EngineService::Restored restored = svc.restore();
        CHECK(restored.loaded && !restored.damaged);
        std::string why;
        CHECK(svc.start(why));
        tick(svc, 1500);
        cascade::core::radioreport::fileSideRadioFailure("rtlsdr", "serial=00000001", "the radio is already in use by another program", "startup-restore");
        tick(svc, 3500);
        o.heartbeatConfigured = svc.engine().heartbeatConfigured();
        svc.stop();
    }
    o.connections = total() - before;
    o.telemetryConnections = g_stand[0]->count() - beforeTel;
    o.seen = summary();
    o.savedHasId = slurp(cfg).find(kId) != std::string::npos;
    return o;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_off_no_heartbeat\n");
    const fs::path root = fs::temp_directory_path() / ("foxsdr_offhb_" + std::to_string(static_cast<long long>(std::time(nullptr))));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "home" / "tmp");
    emt::isolate(root / "home");
    for (int i = 0; i < kN; ++i) {
        g_stand.push_back(std::make_unique<CountingListener>());
        CHECK(g_stand.back()->ok());
        emt::setEnv(kAddresses[i].env, g_stand.back()->https());
    }

    // CONTROL: switch ON with the id - the heartbeat reaches the telemetry stand-in (so a zero below is not a dead stand-in), and the id is kept in the file.
    const Outcome on = runOnce(root / "on", true);
    std::printf("  ON  (control): telemetry connections %d, heartbeatConfigured %d, id kept in file %d, stand-ins: %s\n", on.telemetryConnections, on.heartbeatConfigured ? 1 : 0, on.savedHasId ? 1 : 0, on.seen.c_str());
    CHECK(on.telemetryConnections >= 1);
    CHECK(on.heartbeatConfigured);
    CHECK(on.savedHasId);

    // THE CASE: switch OFF, a valid id left in the file. Nothing connects, nothing is armed, and the file loses the id.
    const int sofar = total();
    const Outcome off = runOnce(root / "off", false);
    std::printf("  OFF (id left in file): connections %d (all stand-ins), telemetry %d, heartbeatConfigured %d, id still in saved file %d\n", total() - sofar, off.telemetryConnections, off.heartbeatConfigured ? 1 : 0, off.savedHasId ? 1 : 0);
    CHECK(total() - sofar == 0);
    CHECK(off.telemetryConnections == 0);
    CHECK(!off.heartbeatConfigured);
    CHECK(!off.savedHasId);

    fs::remove_all(root, ec);
    return testSummary("test_off_no_heartbeat");
}
