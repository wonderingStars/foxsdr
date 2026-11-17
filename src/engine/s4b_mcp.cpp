// s4b_mcp.cpp - see s4b_mcp.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_mcp.hpp"

#include "net/mcp_http.hpp"
#include "net/mcp_server.hpp"

namespace cascade::engine::s4b {

namespace mcp = cascade::net::mcp;
using cascade::net::ControlHub;

namespace {
// The five operations of the model's host interface, over the hub. Nothing else exists.
class HubHost final : public mcp::McpHost {
public:
    explicit HubHost(ControlHub& hub) : hub_(hub) {}
    mcp::StatusInfo status() override {
        const cascade::net::HubStatus s = hub_.status();
        mcp::StatusInfo i;
        i.running = s.running;
        i.centerHz = s.centreHz;
        i.sampleRateHz = s.sampleRateHz;
        i.vfoOffsetHz = s.tunedHz - s.centreHz;
        i.bandwidthHz = s.bandwidthHz;
        i.signalDb = s.signalDb;
        i.mode = s.mode;
        i.sourceName = s.source;
        return i;
    }
    mcp::HostResult tune(double frequencyHz) override {
        const cascade::net::HubResult r = hub_.requestTune(frequencyHz);
        return {r.ok, r.error};
    }
    mcp::HostResult setMode(const std::string& modeName) override {
        const cascade::net::HubResult r = hub_.requestMode(modeName);
        return {r.ok, r.error};
    }
    std::vector<mcp::BookmarkInfo> bookmarks() override {
        std::vector<mcp::BookmarkInfo> out;
        for (const cascade::net::HubBookmark& b : hub_.bookmarks()) { out.push_back({b.name, b.mode, b.group, b.freqHz}); }
        return out;
    }
    std::vector<mcp::DecodeInfo> recentDecodes(std::size_t maxCount) override {
        std::vector<mcp::DecodeInfo> out;
        for (const cascade::net::HubDecode& d : hub_.recentDecodes(maxCount)) { out.push_back({d.unixSeconds, d.freqHz, d.decoder, d.text}); }
        return out;
    }

private:
    ControlHub& hub_;
};
}  // namespace

struct McpEndpoint::Impl {
    explicit Impl(ControlHub& hub, const std::string& token) : host(hub), server(host), policy(server, makeCfg(token)), http(policy) {}
    static mcp::HttpPolicyConfig makeCfg(const std::string& token) {
        mcp::HttpPolicyConfig c;
        c.enabled = true;
        c.path = "/mcp";
        c.bearerToken = token;
        return c;
    }
    HubHost host;
    mcp::McpServer server;
    mcp::McpHttpPolicy policy;
    mcp::McpHttpServer http;
};

McpEndpoint::McpEndpoint() = default;
McpEndpoint::~McpEndpoint() { stop(); }

bool McpEndpoint::start(ControlHub& hub, int port, const std::string& bearerToken, std::string& error) {
    error.clear();
    if (p_ != nullptr) {
        error = "the MCP server is running already";
        return false;
    }
    if (port < 1024 || port > 65535) {
        error = "the port is a whole number from 1024 to 65535";
        return false;
    }
    auto impl = std::make_unique<Impl>(hub, bearerToken);
    switch (impl->http.start("127.0.0.1", port, "/mcp")) {
        case mcp::ListenResult::Started: break;
        case mcp::ListenResult::BindFailed: error = "could not listen on that port (in use, or not allowed)"; return false;
        case mcp::ListenResult::NotLoopback: error = "the MCP server listens on this computer only"; return false;
        case mcp::ListenResult::AlreadyRunning: error = "the MCP server is running already"; return false;
    }
    p_ = std::move(impl);
    return true;
}

void McpEndpoint::stop() {
    if (p_ == nullptr) { return; }
    p_->http.stop();
    p_.reset();
}

bool McpEndpoint::running() const { return p_ != nullptr && p_->http.running(); }
int McpEndpoint::port() const { return p_ != nullptr ? p_->http.port() : 0; }

}  // namespace cascade::engine::s4b
