// s4b_mcp.hpp - screen lane S4b, G3-32: the MCP server of net/mcp_server.hpp and net/mcp_http.hpp, wired to the control hub (net/control_hub.hpp).
//
// WHAT AN MCP CLIENT MAY DO is the model's closed list of five tools, and nothing else: get_status, list_bookmarks, recent_decodes (read) and tune, set_mode (the receiver's frequency and
// demodulator mode). The host interface the model asks for has exactly those five members, and this adapter implements them over the hub, which validates, rate-limits (ten a second) and
// queues; the engine's pass applies the tuning through its ordinary commands. There is no path from here to transmit, key a radio, write or read a file, change a setting, start or stop anything,
// open a connection or run a program.
//
// LOOPBACK ONLY, BY CONSTRUCTION: net/mcp_http.hpp refuses any bind address that is not 127.0.0.0/8, ::1 or localhost, and checks the Host and Origin headers of every request against that
// (a web page cannot reach it: DNS rebinding and cross-site requests are refused). An optional bearer token (kept in the Windows protected store) makes every request carry
// "Authorization: Bearer <token>". Off until started.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <memory>
#include <string>

#include "net/control_hub.hpp"

namespace cascade::engine::s4b {

class McpEndpoint {
public:
    McpEndpoint();
    ~McpEndpoint();
    McpEndpoint(const McpEndpoint&) = delete;
    McpEndpoint& operator=(const McpEndpoint&) = delete;

    // Binds 127.0.0.1:port and serves /mcp. False with a sentence in `error` (a port in use, a bad port). `bearerToken` "" = none.
    bool start(cascade::net::ControlHub& hub, int port, const std::string& bearerToken, std::string& error);
    void stop();
    bool running() const;
    int port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace cascade::engine::s4b
