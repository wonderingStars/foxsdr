// test_engine_api_s4b_control.cpp - screen lane S4b's WebSocket / SSE API and MCP server through the REAL engine: a stand-in radio (a tone source) is open; the wirec settings turn the servers on;
// stand-in clients (plain loopback sockets and the vendored HTTP client) read the status, retune the radio, set the mode and read decoded lines. Everything binds 127.0.0.1 (a wider bind raises
// the firewall prompt: "every address" is only proved REFUSED, while the web page has no password, before any socket is made). No radio is opened; nothing leaves this computer.
//
//   A1  off by default; bad ports refused; the MCP token is a secret (never a fact, never in wirec.json)
//   A2  the API on this computer: the status comes from the engine, a WebSocket tune and set_mode reach the engine's own tuning and mode, decoded lines arrive (a private plugin's never),
//       "listen on every address" is refused while the web page has no password
//   A3  once the web page has a password the API asks for it: no credentials 401, wrong 401, right 200, and the sixth wrong attempt is told to wait
//   A4  the MCP server: exactly five tools; tune reaches the engine; an unknown tool (transmit) is refused; a token makes every request carry it
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include "net/ws_frame.hpp"
#include "net_test_support.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
using nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

class Tone final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 240000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return center_; }
    bool setCenterFrequencyHz(double hz) override { center_ = hz; return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double a = 2.0 * kPi * 15000.0 * static_cast<double>(k_) / 240000.0;
            dst[i] = {static_cast<float>(0.3 * std::cos(a)), static_cast<float>(0.3 * std::sin(a))};
        }
        return n;
    }
    const char* name() const override { return "s4b-tone"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double center_ = 100.0e6;
    std::uint64_t k_ = 0;
};

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

int freePort() {
    std::string err;
    std::uint32_t ip = 0;
    cascade::net::sock::parseIpv4("127.0.0.1", ip);
    const cascade::net::sock::Handle h = cascade::net::sock::listenTcp(ip, 0, 1, err);
    const int p = h == cascade::net::sock::kInvalid ? 0 : cascade::net::sock::boundPort(h);
    cascade::net::sock::closeHandle(h);
    return p;
}

std::string wsFrame(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    f.push_back(static_cast<char>(0x80 | payload.size()));   // (every message here is under 126 bytes)
    const std::uint8_t key[4] = {1, 2, 3, 4};
    f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) { f.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4])); }
    return f;
}

bool contains(nettest::Client& c, const std::string& s) { return std::string(c.got.begin(), c.got.end()).find(s) != std::string::npos; }

bool pumpFor(Rig& rig, nettest::Client& c, const std::string& marker, int ms = 6000) {
    const auto t0 = nettest::Clock::now();
    while (nettest::msSince(t0) < ms) {
        rig.pass(2);
        c.pump(30);
        if (contains(c, marker)) { return true; }
    }
    return contains(c, marker);
}

const char* kWsRequest = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

std::string httpGet(Rig& rig, int port, const std::string& extraHeaders, int* status = nullptr) {
    nettest::Client c;
    if (!c.connect(static_cast<std::uint16_t>(port))) { return "<no connection>"; }
    const std::string req = "GET /status HTTP/1.1\r\nHost: x\r\n" + extraHeaders + "\r\n";
    c.sendBytes(req.data(), req.size());
    const auto t0 = nettest::Clock::now();
    while (!c.eof && nettest::msSince(t0) < 5000) { rig.pass(1); c.pump(30); }
    const std::string all(c.got.begin(), c.got.end());
    if (status != nullptr) { *status = all.size() > 12 ? std::atoi(all.substr(9, 3).c_str()) : 0; }
    const std::size_t b = all.find("\r\n\r\n");
    return b == std::string::npos ? all : all.substr(b + 4);
}

std::string slurp(const fs::path& p) { return readAll(p); }

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_control");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("s4bctl");
    {
        Rig rig(dir);
        Access::useSource(rig.eng, std::make_unique<Tone>());
        CHECK(rig.status(testing::cmd(FOXAPI_OP_RUN, 0, 1)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6)) == FOXAPI_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        rig.pass(4);

        // A1
        CHECK(rig.fact("srv.api.running") == "0" && rig.fact("srv.mcp.running") == "0" && rig.fact("srv.api.port").empty() && rig.fact("srv.api.scope").empty() && rig.fact("srv.mcp.scope").empty());
        CHECK(rig.fact("wirec.srv.api.enabled") == "0" && rig.fact("wirec.srv.api.all") == "0" && rig.fact("wirec.srv.api.port") == "8074");
        CHECK(rig.fact("wirec.srv.mcp.enabled") == "0" && rig.fact("wirec.srv.mcp.port") == "8075" && rig.fact("wirec.srv.mcp.token").empty());
        CHECK(rig.status(setting("srv.api.port", "80")) != FOXAPI_OK && rig.status(setting("srv.mcp.port", "70000")) != FOXAPI_OK && rig.status(setting("srv.mcp.token", "a b")) != FOXAPI_OK);
        CHECK(rig.status(setting("srv.mcp.token", "tok-unique-s4b")) == FOXAPI_OK);
        CHECK(rig.fact("wirec.srv.mcp.token") == "set" && slurp(dir / "wirec.json").find("tok-unique-s4b") == std::string::npos);
        const int apiPort = freePort();
        CHECK(rig.status(setting("srv.api.port", std::to_string(apiPort))) == FOXAPI_OK);

        // A2
        CHECK(rig.status(setting("srv.api.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.api.running") == "1"; }));
        CHECK(rig.fact("srv.api.port") == std::to_string(apiPort) && rig.fact("srv.api.scope") == "this computer only");
        {
            int st = 0;
            const std::string body = httpGet(rig, apiPort, "", &st);
            const json j = json::parse(body, nullptr, false);
            CHECK(st == 200 && j.is_object() && std::fabs(j.value("centre_hz", 0.0) - 100.0e6) < 1.0 && j.value("source", "") == "s4b-tone" && j.value("sample_rate_hz", 0.0) == 240000.0);
        }
        nettest::Client ws;
        CHECK(ws.connect(static_cast<std::uint16_t>(apiPort)));
        ws.sendBytes(kWsRequest, std::strlen(kWsRequest));
        CHECK(pumpFor(rig, ws, "\"type\":\"status\""));
        // a decoded line: before the subscription nothing; after it the public plugin's line arrives, a private plugin's never
        const std::string sub = wsFrame(R"({"op":"subscribe","decodes":true})");
        ws.sendBytes(sub.data(), sub.size());
        CHECK(pumpFor(rig, ws, "\"type\":\"ok\""));
        rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "hello from a decoder", 148.0e6, 1000000);
        rig.eng.pluginApiShared()->hostPostEvent("secret-decoder", true, "text", "a private line", 148.0e6, 1001000);
        CHECK(pumpFor(rig, ws, "hello from a decoder"));
        rig.pass(10);
        ws.pump(300);
        CHECK(!contains(ws, "a private line"));
        // tune and set_mode reach the engine's own commands
        const std::string t1 = wsFrame(R"({"op":"tune","hz":150000000})");
        ws.sendBytes(t1.data(), t1.size());
        CHECK(until(rig, [&] { return std::fabs(rig.local->state().tunedHz - 150.0e6) < 1.0; }));
        CHECK(until(rig, [&] { return rig.fact("srv.api.tunes") == "1"; }));
        const std::string m1 = wsFrame(R"({"op":"set_mode","mode":"usb"})");
        ws.sendBytes(m1.data(), m1.size());
        CHECK(until(rig, [&] { return rig.local->state().demodMode == FOXAPI_DEMOD_USB; }));
        // a bad request changes nothing
        const double before = rig.local->state().tunedHz;
        const std::string bad = wsFrame(R"({"op":"tune","hz":5})");
        ws.sendBytes(bad.data(), bad.size());
        const std::string bad2 = wsFrame(R"({"op":"transmit"})");
        ws.sendBytes(bad2.data(), bad2.size());
        CHECK(pumpFor(rig, ws, "unknown operation"));
        rig.pass(8);
        CHECK(rig.local->state().tunedHz == before);
        ws.close();
        // every address: refused while the web page has no password (and no socket is made)
        CHECK(rig.status(setting("srv.api.all", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return !rig.fact("srv.api.problem").empty(); }));
        CHECK(rig.fact("srv.api.running") == "0" && rig.fact("srv.api.problem").find("password for the web page") != std::string::npos);
        CHECK(rig.status(setting("srv.api.all", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.api.running") == "1"; }));

        // A3: the web page's password guards the API
        CHECK(rig.status(textOp(FOXAPI_OP_SERVER_CONFIG, R"({"web":{"password":"correct horse battery"}})")) == FOXAPI_OK);
        const std::string good = "Authorization: Basic " + cascade::net::ws::base64Encode("admin:correct horse battery") + "\r\n";
        const std::string wrong = "Authorization: Basic " + cascade::net::ws::base64Encode("admin:not the password") + "\r\n";
        int st = 0;
        bool guarded = false;
        for (int i = 0; i < 80 && !guarded; ++i) {   // (the password is hashed on a worker: until it is in force the API is open)
            httpGet(rig, apiPort, "", &st);
            guarded = st == 401;
            if (!guarded) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        }
        CHECK(guarded);
        httpGet(rig, apiPort, good, &st);
        CHECK(st == 200);
        for (int i = 0; i < 5; ++i) { httpGet(rig, apiPort, wrong, &st); CHECK(st == 401); }
        httpGet(rig, apiPort, wrong, &st);
        CHECK(st == 429);
        httpGet(rig, apiPort, good, &st);   // a verified login is remembered: the good client is not locked out by the bad one's attempts
        CHECK(st == 200);

        // A4: the MCP server
        const int mcpPort = freePort();
        CHECK(rig.status(setting("srv.mcp.port", std::to_string(mcpPort))) == FOXAPI_OK);
        CHECK(rig.status(setting("srv.mcp.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.mcp.running") == "1"; }));
        CHECK(rig.fact("srv.mcp.port") == std::to_string(mcpPort) && rig.fact("srv.mcp.scope") == "this computer only");
        {
            httplib::Client cli("127.0.0.1", mcpPort);
            cli.set_read_timeout(5, 0);
            const json meta = {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", json::object()}};
            const auto post = [&](const std::string& method, json params, const std::string& name, bool bearer) {
                params["_meta"] = meta;
                const json body = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
                httplib::Headers h{{"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", method}, {"Accept", "application/json"}};
                if (!name.empty()) { h.emplace("Mcp-Name", name); }
                if (bearer) { h.emplace("Authorization", "Bearer tok-unique-s4b"); }
                std::thread pump([&] { for (int i = 0; i < 40; ++i) { rig.pass(1); std::this_thread::sleep_for(std::chrono::milliseconds(20)); } });
                const auto r = cli.Post("/mcp", h, body.dump(), "application/json");
                pump.join();
                return r ? std::make_pair(r->status, r->body) : std::make_pair(-1, std::string());
            };
            auto r = post("tools/list", json::object(), "", false);
            CHECK(r.first == 401);   // the token is set: a request without it is refused
            r = post("tools/list", json::object(), "", true);
            const json tl = json::parse(r.second, nullptr, false);
            CHECK(r.first == 200 && tl.is_object() && tl["result"]["tools"].size() == 5);
            std::string names;
            if (tl.is_object()) { for (const json& t : tl["result"]["tools"]) { names += t.value("name", "") + ","; } }
            CHECK(names == "get_status,list_bookmarks,recent_decodes,tune,set_mode,");
            r = post("tools/call", {{"name", "tune"}, {"arguments", {{"frequency_hz", 99.5e6}}}}, "tune", true);
            CHECK(r.first == 200);
            CHECK(until(rig, [&] { return std::fabs(rig.local->state().tunedHz - 99.5e6) < 1.0; }));
            r = post("tools/call", {{"name", "transmit"}, {"arguments", json::object()}}, "transmit", true);
            const json tx = json::parse(r.second, nullptr, false);
            CHECK(tx.is_object() && tx.contains("error"));
            r = post("tools/call", {{"name", "get_status"}, {"arguments", json::object()}}, "get_status", true);
            CHECK(r.first == 200 && r.second.find("s4b-tone") != std::string::npos);
        }
        // everything off again: the ports are released
        CHECK(rig.status(setting("srv.mcp.enabled", "0")) == FOXAPI_OK && rig.status(setting("srv.api.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.mcp.running") == "0" && rig.fact("srv.api.running") == "0"; }));
        nettest::Client gone, gone2;
        CHECK(!gone.connect(static_cast<std::uint16_t>(apiPort), 300) && !gone2.connect(static_cast<std::uint16_t>(mcpPort), 300));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_s4b_control");
}
