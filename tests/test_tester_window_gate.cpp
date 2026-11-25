// THE CLASSIC WINDOW'S TESTER REPORT OBEYS USAGE REPORTING, THROUGH THE WINDOW'S OWN CALLS (0.99.115, brief 79; reviews 73 of lane 62: S4 and B2).
//
// The pure function (testerUsageMayPost) was tested before; deleting the CALL to it in app_window_tester.cpp kept every test green. These cases drive the real AppWindow functions against a
// loopback listener that counts every connection (the report address and the portal-link address are both redirected to it), with the engine's real Usage switch behind the fact:
//
//   S4a  Usage OFF, a code set, a report waiting: testerUsagePoll() makes NO connection, however often it is asked;  ON: exactly one
//   S4b  Usage OFF, an old portal token and no app token: testerLinkPoll() starts no exchange;  ON: it does
//   B2a  Usage OFF: the session report is not built, not written to config.json (testerUsageCurrent) and the queue is emptied (testerUsagePending, in memory)
//   B2b  the ON edge starts from empty: what was collected while OFF is gone, and the session's start is the ON
//   B2c  Usage ON throughout: the report is built and written exactly as in 0.99.114
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

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/tester_usage.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace cascade::gui {
struct AppWindowTestAccess {
    static void pump(AppWindow& a) { passseam::whole(th::engine(a), [&] { th::engine(a).pump(); }); }
    static void consent(AppWindow& a, int on) { passseam::on(th::engine(a), [&] { (void)th::engine(a).applyCommand(cascade::core::cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, on)); }); }
    static void usagePoll(AppWindow& a) { a.testerUsagePoll(); }
    static void linkPoll(AppWindow& a) { a.testerLinkPoll(); }
    static void setCode(AppWindow& a, const std::string& token) { a.setTesterToken(token, false); }
    static void setPortalOnly(AppWindow& a, const std::string& token) {
        a.testerToken_ = token;
        a.testerAppToken_.clear();
        a.testerMigrationTried_ = false;
        a.testerUsage_->setArmed(true);
    }
    static void queue(AppWindow& a, const std::string& json) { a.testerUsageQueue_.push(json); }
    static std::size_t queued(AppWindow& a) { return a.testerUsageQueue_.size(); }
    static void note(AppWindow& a, const char* feature) { a.testerUsage_->noteFeature(feature); }
    static std::vector<std::string> features(AppWindow& a) { return a.testerUsage_->features(); }
    static cascade::core::AppConfig save(AppWindow& a) {
        cascade::core::AppConfig c;
        a.testerUsageJournal(c, /*bookkeeping=*/true);
        return c;
    }
    static void rewindSession(AppWindow& a, double seconds) { a.testerSessionStart_ -= seconds; }
    static double sessionStart(AppWindow& a) { return a.testerSessionStart_; }
    static void triedThisSession(AppWindow& a, bool v) { a.testerUsageQueueTriedThisSession_ = v; }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

#if defined(_WIN32)
// A listener on 127.0.0.1 that counts every connection and answers none.
class CountServer {
public:
    bool start() {
        WSADATA w;
        ::WSAStartup(MAKEWORD(2, 2), &w);
        s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == INVALID_SOCKET) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { return false; }
        int len = static_cast<int>(sizeof(a));
        if (::getsockname(s_, reinterpret_cast<sockaddr*>(&a), &len) != 0) { return false; }
        port_ = ntohs(a.sin_port);
        if (::listen(s_, 8) != 0) { return false; }
        thread_ = std::thread([this] {
            for (;;) {
                SOCKET c = ::accept(s_, nullptr, nullptr);
                if (c == INVALID_SOCKET) { break; }
                std::lock_guard<std::mutex> lk(mu_);
                held_.push_back(c);
                accepted_.fetch_add(1);
            }
        });
        return true;
    }
    ~CountServer() {
        if (s_ != INVALID_SOCKET) { ::closesocket(s_); }
        if (thread_.joinable()) { thread_.join(); }
        for (SOCKET c : held_) { ::closesocket(c); }
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }
    int accepted() const { return accepted_.load(); }
private:
    SOCKET s_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::vector<SOCKET> held_;
    std::atomic<int> accepted_{0};
};
#endif

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

const std::string kCode = "0123456789abcdef0123456789abcdef";   // a code of the shape validTesterToken accepts (32 hex)

std::string reportFor(const std::string& token) { return std::string("{\"token\":\"") + token + "\",\"version\":\"0.99.115\"}"; }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_tester_window_gate\n");
#if !defined(_WIN32)
    std::printf("  (Windows only: the loopback counter is a WinSock listener)\n");
    return testSummary("test_tester_window_gate");
#else
    const fs::path scratch = fs::temp_directory_path() / ("cascade-tester-gate-" + std::to_string(_getpid()));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    setEnv("CASCADE_CONFIG_TEST", "1");
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, scratch.string()); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_DIAG_DIR", scratch.string());
    CountServer server;
    CHECK(server.start());
    setEnv("FOXSDR_TESTER_USAGE_URL", server.url() + "api/tester-usage");
    setEnv("FOXSDR_BETA_API_URL", server.url());
    ImGui::CreateContext();

    // --- S4a: the report's own sender -------------------------------------------------------------------------------------------------------------
    {
        std::printf("  S4a Usage OFF: the window's poll makes no connection; Usage ON: one\n");
        th::TestWindow app;
        A::setCode(app, kCode);
        A::queue(app, reportFor(kCode));
        A::pump(app);                                        // the engine's first publish: fact.telemetry.enabled is "0" (a hermetic engine: nothing consented)
        for (int i = 0; i < 40; ++i) { A::usagePoll(app); }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(server.accepted() == 0);                       // (delete the gate's call in testerUsagePoll and this is 1)
        CHECK(A::queued(app) == 0);                          // the report of the OFF period is gone (rule R), so it can never be sent after the ON
        A::queue(app, reportFor(kCode));                     // something put back into the queue while the switch is OFF (no edge now: nothing empties it)
        for (int i = 0; i < 40; ++i) { A::usagePoll(app); }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(server.accepted() == 0);                       // the poll's own gate holds it (delete the gate and this is 1; the edge's emptying is not what is tested here)
        CHECK(A::queued(app) == 1);
        A::consent(app, 1);
        A::pump(app);
        A::usagePoll(app);                                   // the ON edge
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(server.accepted() == 0);                       // nothing is left to send
        A::queue(app, reportFor(kCode));                     // a session report of the ON period
        for (int i = 0; i < 4; ++i) { A::usagePoll(app); }
        CHECK(waitFor([&] { return server.accepted() >= 1; }));
        CHECK(server.accepted() == 1);
    }
    // --- S4b: the portal-link exchange of a launch ------------------------------------------------------------------------------------------------
    {
        std::printf("  S4b Usage OFF: no portal-link exchange at launch; Usage ON: it starts\n");
        const int before = server.accepted();
        th::TestWindow app;
        A::setPortalOnly(app, kCode);
        A::pump(app);
        for (int i = 0; i < 40; ++i) { A::linkPoll(app); }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(server.accepted() == before);                  // (delete the gate's call in testerLinkPoll and this moves)
        A::consent(app, 1);
        A::pump(app);
        for (int i = 0; i < 4; ++i) { A::linkPoll(app); }
        CHECK(waitFor([&] { return server.accepted() > before; }));
    }
    // --- B2a / B2b / B2c: nothing is built, written or queued while OFF; the ON edge starts empty -------------------------------------------------
    {
        std::printf("  B2  Usage OFF: no session report in config.json, no queue; the ON edge starts empty\n");
        th::TestWindow app;
        A::setCode(app, kCode);
        A::queue(app, reportFor(kCode));
        A::note(app, "waterfall");
        A::pump(app);                                        // fact "0"
        A::usagePoll(app);                                   // the window follows the switch: OFF
        CHECK(A::queued(app) == 0);                          // the queue is emptied
        {
            const cascade::core::AppConfig c = A::save(app);
            CHECK(c.testerUsageCurrent.empty());             // the report so far is not built and not written
            CHECK(c.testerUsagePending.empty());
        }
        A::note(app, "spectrum");                            // activity in the OFF period
        CHECK(A::features(app).empty());                     // the collector is not armed: nothing is recorded
        A::rewindSession(app, 900.0);                        // the session is 15 minutes old when it is switched on
        const double startOff = A::sessionStart(app);
        A::consent(app, 1);
        A::pump(app);
        A::usagePoll(app);                                   // the ON edge
        CHECK(A::features(app).empty());                     // starts from empty
        CHECK(A::sessionStart(app) > startOff + 800.0);      // the session of the ON period begins at the ON
        A::note(app, "decoder");
        {
            const cascade::core::AppConfig c = A::save(app);
            CHECK(!c.testerUsageCurrent.empty());            // B2c: ON, the report is built and written as before
            CHECK(c.testerUsageCurrent.find("decoder") != std::string::npos);
            CHECK(c.testerUsageCurrent.find("waterfall") == std::string::npos && c.testerUsageCurrent.find("spectrum") == std::string::npos);
        }
        A::consent(app, 0);                                  // OFF again, mid-session: what the ON period collected goes too
        A::pump(app);
        A::usagePoll(app);
        {
            const cascade::core::AppConfig c = A::save(app);
            CHECK(c.testerUsageCurrent.empty() && c.testerUsagePending.empty());
        }
        CHECK(A::features(app).empty());
    }

    fs::remove_all(scratch, ec);
    return testSummary("test_tester_window_gate");
#endif
}
