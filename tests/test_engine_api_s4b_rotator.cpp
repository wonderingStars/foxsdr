// test_engine_api_s4b_rotator.cpp - screen lane S4b, G3-20: the antenna rotator through the REAL engine. The rotctld daemon is a stand-in server on 127.0.0.1 (a thread of this test that speaks the
// protocol's lines); the GS-232 controller is a stand-in byte stream handed to the engine in place of a serial port: NO COM PORT IS OPENED by this test and no real rotator or program is contacted.
//
//   P1  off by default: no connection is made, the ops are refused with a sentence
//   P2  rotctld: the position is read back, a turn / stop / park reach the daemon as its own commands, a bad bearing is refused before anything is sent, a daemon that goes away is said
//   P3  a GS-232 controller (stand-in): the turn is the three-digit command, a serial name that is not a COM name is refused by the settings and never reaches the factory
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "net_test_support.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

namespace sock = cascade::net::sock;

// A stand-in rotctld: one connection at a time, the lines of the protocol, a position it keeps.
class FakeRotctld {
public:
    bool start() {
        std::string err;
        std::uint32_t ip = 0;
        sock::parseIpv4("127.0.0.1", ip);
        listener_ = sock::listenTcp(ip, 0, 4, err);
        if (listener_ == sock::kInvalid) { return false; }
        port_ = sock::boundPort(listener_);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    ~FakeRotctld() { stop(); }
    void stop() {
        if (!th_.joinable()) { return; }
        stop_.store(true);
        th_.join();
        sock::closeHandle(listener_);
        listener_ = sock::kInvalid;
    }
    // drops the connection in force (the daemon "went away")
    void dropClient() { drop_.store(true); }
    int port() const { return port_; }
    int accepted() const { return accepted_.load(); }
    std::string received() {
        std::lock_guard<std::mutex> g(m_);
        return log_;
    }

private:
    void loop() {
        sock::Handle client = sock::kInvalid;
        std::string buf;
        double az = 10.0, el = 20.0;
        while (!stop_.load()) {
            if (client == sock::kInvalid) {
                sock::PollItem l;
                l.s = listener_;
                l.want = sock::kReadable;
                if (sock::pollMany(&l, 1, 20) > 0 && (l.got & sock::kReadable)) {
                    client = sock::acceptOne(listener_);
                    if (client != sock::kInvalid) { accepted_.fetch_add(1); buf.clear(); }
                }
                continue;
            }
            if (drop_.exchange(false)) { sock::closeHandle(client); client = sock::kInvalid; continue; }
            sock::PollItem it;
            it.s = client;
            it.want = sock::kReadable;
            if (sock::pollMany(&it, 1, 20) <= 0) { continue; }
            char b[256];
            const long n = sock::recvSome(client, b, sizeof b);
            if (n == 0 || (n < 0 && !sock::wouldBlock())) { sock::closeHandle(client); client = sock::kInvalid; continue; }
            if (n > 0) { buf.append(b, static_cast<std::size_t>(n)); }
            for (std::size_t p; (p = buf.find('\n')) != std::string::npos;) {
                const std::string line = buf.substr(0, p);
                buf.erase(0, p + 1);
                { std::lock_guard<std::mutex> g(m_); log_ += line + "\n"; }
                std::string reply;
                if (line.rfind("P ", 0) == 0) { std::sscanf(line.c_str(), "P %lf %lf", &az, &el); reply = "RPRT 0\n"; }
                else if (line == "p") { char t[64]; std::snprintf(t, sizeof t, "%.2f\n%.2f\n", az, el); reply = t; }
                else if (line == "S" || line == "K") { reply = "RPRT 0\n"; }
                else { reply = "RPRT -1\n"; }
                sock::sendSome(client, reply.data(), reply.size());
            }
        }
        if (client != sock::kInvalid) { sock::closeHandle(client); }
    }
    sock::Handle listener_ = sock::kInvalid;
    std::thread th_;
    std::atomic<bool> stop_{false}, drop_{false};
    std::atomic<int> accepted_{0};
    int port_ = 0;
    std::mutex m_;
    std::string log_;
};

// A stand-in GS-232 controller: every command is acknowledged with a bare CR; "C2" is answered with a position.
class FakeGs232 final : public cascade::core::rotator::Transport {
public:
    explicit FakeGs232(std::shared_ptr<std::string> log) : log_(std::move(log)) {}
    bool write(std::string_view bytes) override {
        last_.assign(bytes);
        { std::lock_guard<std::mutex> g(mu); *log_ += std::string(bytes); }
        return true;
    }
    bool readUntil(char delim, std::string* out, int) override {
        if (delim != '\r') { return false; }
        out->clear();
        if (last_ == "C2\r") { *out = "+0090+0045"; }
        return true;
    }
    static std::mutex mu;

private:
    std::shared_ptr<std::string> log_;
    std::string last_;
};
std::mutex FakeGs232::mu;

template <class F>
bool until(Rig& r, F f, int ms = 8000) {
    const auto t0 = nettest::Clock::now();
    while (nettest::msSince(t0) < ms) {
        r.pass(2);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return f();
}

FoxCommand rotOp(const std::string& json) { return textOp(FOXAPP_OP_ROTATOR, json); }

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_rotator");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("s4brot");
    {
        FakeRotctld daemon;
        CHECK(daemon.start());
        Rig rig(dir);
        std::string msg;
        // P1
        CHECK(rig.fact("rot.running") == "0" && rig.fact("rot.connected") == "0" && rig.fact("rot.scope").empty());
        CHECK(rig.fact("wirec.rot.enabled") == "0" && rig.fact("wirec.rot.kind") == "rotctld" && rig.fact("wirec.rot.host") == "127.0.0.1" && rig.fact("wirec.rot.port") == "4533");
        CHECK(rig.status(rotOp(R"({"action":"turn","az":90,"el":10})"), &msg) == FOXAPI_FAILED && msg.find("off") != std::string::npos);
        CHECK(rig.status(rotOp(R"({"action":"stop"})")) == FOXAPI_FAILED && rig.status(rotOp(R"({"action":"park"})")) == FOXAPI_FAILED);
        rig.pass(10);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(daemon.accepted() == 0);   // nothing was ever dialled
        CHECK(rig.status(rotOp("not json")) == FOXAPI_BAD_ARGUMENT && rig.status(rotOp(R"({"action":"launch"})")) == FOXAPI_BAD_ARGUMENT && rig.status(rotOp(R"({"action":"turn","az":1})")) == FOXAPI_BAD_ARGUMENT);
        CHECK(rig.status(rotOp(R"({"action":"stop","az":1})")) == FOXAPI_BAD_ARGUMENT && rig.status(rotOp(R"({"action":"turn","az":"x","el":1})")) == FOXAPI_BAD_ARGUMENT);
        CHECK(rig.status(setting("rot.host", "bad host")) != FOXAPI_OK && rig.status(setting("rot.serial", "C:\\x.txt")) != FOXAPI_OK && rig.status(setting("rot.serial", "COM3")) == FOXAPI_OK);
        CHECK(rig.status(setting("rot.serial", "")) == FOXAPI_OK);

        // P2
        CHECK(rig.status(setting("rot.port", std::to_string(daemon.port()))) == FOXAPI_OK);
        CHECK(rig.status(setting("rot.az_max", "360")) == FOXAPI_OK);
        CHECK(rig.status(setting("rot.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.connected") == "1"; }));
        CHECK(rig.fact("rot.scope") == "this computer only");
        CHECK(until(rig, [&] { return rig.fact("rot.az") == "10.0" && rig.fact("rot.el") == "20.0"; }));   // where the controller says the antenna is
        CHECK(rig.status(rotOp(R"({"action":"turn","az":123.4,"el":56})")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.turns") == "1"; }));
        CHECK(daemon.received().find("P 123.40 56.00\n") != std::string::npos);
        CHECK(until(rig, [&] { return rig.fact("rot.az") == "123.4" && rig.fact("rot.el") == "56.0"; }));
        // a bearing the controller does not take is refused before anything is sent
        const std::string before = daemon.received();
        CHECK(rig.status(rotOp(R"({"action":"turn","az":361,"el":10})")) == FOXAPI_OUT_OF_RANGE);
        CHECK(rig.status(rotOp(R"({"action":"turn","az":-1,"el":10})")) == FOXAPI_OUT_OF_RANGE);
        CHECK(rig.status(rotOp(R"({"action":"turn","az":10,"el":181})")) == FOXAPI_OUT_OF_RANGE);
        rig.pass(6);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(daemon.received().find("P 361") == std::string::npos && daemon.received().find("P -1") == std::string::npos && daemon.received().find("P 10.00 181") == std::string::npos);
        (void)before;
        CHECK(rig.status(rotOp(R"({"action":"stop"})")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.stops") == "1"; }));
        CHECK(rig.status(rotOp(R"({"action":"park"})")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.parks") == "1"; }));
        CHECK(daemon.received().find("\nS\n") != std::string::npos && daemon.received().find("\nK\n") != std::string::npos);
        // the daemon goes away: said in a sentence, nothing crashes, and the link is tried again
        daemon.dropClient();
        daemon.stop();
        CHECK(until(rig, [&] { return rig.fact("rot.connected") == "0" && !rig.fact("rot.problem").empty(); }, 12000));
        CHECK(rig.status(rotOp(R"({"action":"turn","az":10,"el":10})")) == FOXAPI_OK);   // accepted for the worker; it will fail and say so
        CHECK(until(rig, [&] { return std::atoi(rig.fact("rot.failures").c_str()) >= 1; }, 12000));
        CHECK(rig.status(setting("rot.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.running") == "0"; }));
        CHECK(rig.status(rotOp(R"({"action":"stop"})")) == FOXAPI_FAILED);

        // P3: a GS-232 on a stand-in stream (the factory is the test's: no port is opened)
        auto log = std::make_shared<std::string>();
        std::atomic<int> factoryCalls{0};
        std::string lastComPort;
        std::mutex lm;
        Access::wirec(rig.eng).servers.rotator().setFactory([&](const cascade::engine::s4b::RotatorConfig& c, std::string& err) -> std::unique_ptr<cascade::core::rotator::Transport> {
            ++factoryCalls;
            { std::lock_guard<std::mutex> g(lm); lastComPort = c.comPort; }
            if (!c.serial) { err = "this test only gives a GS-232"; return nullptr; }
            return std::make_unique<FakeGs232>(log);
        });
        CHECK(rig.status(setting("rot.kind", "gs232")) == FOXAPI_OK);
        CHECK(rig.status(setting("rot.serial", "COM7")) == FOXAPI_OK);
        CHECK(rig.status(setting("rot.az_max", "450")) == FOXAPI_OK);
        CHECK(rig.status(setting("rot.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.connected") == "1"; }));
        CHECK(rig.fact("rot.scope") == "a serial port of this computer");
        { std::lock_guard<std::mutex> g(lm); CHECK(lastComPort == "COM7"); }
        CHECK(until(rig, [&] { return rig.fact("rot.az") == "90.0" && rig.fact("rot.el") == "45.0"; }));
        CHECK(rig.status(rotOp(R"({"action":"turn","az":123.4,"el":56})")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.turns") == "2"; }));   // (the counts run on from the rotctld part)
        { std::lock_guard<std::mutex> g(FakeGs232::mu); CHECK(log->find("W123 056\r") != std::string::npos); }
        CHECK(rig.status(rotOp(R"({"action":"turn","az":400,"el":10})")) == FOXAPI_OK);   // 450 is this controller's range
        CHECK(rig.status(rotOp(R"({"action":"turn","az":451,"el":10})")) == FOXAPI_OUT_OF_RANGE);
        CHECK(rig.status(rotOp(R"({"action":"stop"})")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.stops") == "2"; }));
        { std::lock_guard<std::mutex> g(FakeGs232::mu); CHECK(log->find("S\r") != std::string::npos); }
        // a name that is not a COM name never gets as far as the factory
        const int callsBefore = factoryCalls.load();
        CHECK(rig.status(setting("rot.serial", "\\\\.\\pipe\\x")) != FOXAPI_OK && rig.status(setting("rot.serial", "COM")) != FOXAPI_OK && rig.status(setting("rot.serial", "COM1x")) != FOXAPI_OK);
        rig.pass(6);
        CHECK(factoryCalls.load() == callsBefore);
        CHECK(rig.status(setting("rot.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("rot.running") == "0"; }));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_s4b_rotator");
}
