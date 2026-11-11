// mcp_server.hpp - a Model Context Protocol server core: a JSON-RPC 2.0 handler that exposes FIVE tools (three read-only, two that tune)
// and nothing else, plus the Streamable HTTP request policy for a loopback endpoint (row N16 of the gaps survey). Header-only, no HTTP
// library, no thread: net/mcp_http.hpp is the loopback listener. Not wired (see the record's WIRING HAND-OFF). OFF BY DEFAULT.
//
// SPECIFICATION READ (https://modelcontextprotocol.io/specification/latest, revision 2026-07-28 at the time of reading, 2026-11-11):
// the pages Overview, Base protocol, Transports > Streamable HTTP, Versioning, Server > Tools and Server > Discovery. What this
// revision says and what is built (everything below is from those pages):
//  * Stateless: no initialize handshake. Every request carries params._meta with "io.modelcontextprotocol/protocolVersion" (string) and
//    "io.modelcontextprotocol/clientCapabilities" (object); a missing field is -32602 Invalid params, HTTP 400. A version the server does
//    not implement is -32022 UnsupportedProtocolVersion with data.supported and data.requested, HTTP 400. Servers MUST implement
//    server/discover, whose result carries supportedVersions, capabilities, _meta serverInfo and instructions.
//  * Messages: JSON-RPC 2.0, one request or notification per message (no batches), request ids are strings or integers and never null; a
//    result carries "resultType": "complete". A notification gets HTTP 202 with no body. Unknown method: HTTP 404 with -32601.
//  * Streamable HTTP: a single endpoint that accepts POST (GET and DELETE: 405); the client sends Accept with application/json and
//    text/event-stream; the server MUST validate the Origin header (403 if present and invalid) and SHOULD bind only to 127.0.0.1 and
//    authenticate. Every POST carries MCP-Protocol-Version (must equal the _meta value: else 400 with -32020 HeaderMismatch), Mcp-Method
//    (must equal the body's method) and, for tools/call, Mcp-Name (must equal params.name; a value outside plain ASCII is sent as
//    "=?base64?...?=" and is decoded before comparing). The server answers each request with one application/json object (no SSE stream
//    is ever opened). Notifications: "header requirements for notification POSTs are not defined by this revision": none are required.
//  * Tools: tools/list returns {resultType, tools:[{name, title, description, inputSchema, annotations}]} in a deterministic order;
//    tools/call returns {resultType, content:[{type:"text", text}], structuredContent, isError}. An unknown tool and a malformed call are
//    JSON-RPC errors (-32602); a failure while running a tool is a result with isError true. Servers MUST validate tool inputs, rate-limit
//    tool calls and sanitise tool outputs (all three done here). Tool annotations (readOnlyHint, destructiveHint, idempotentHint,
//    openWorldHint) are named on the page only as "optional properties describing tool behavior": the four names are from earlier
//    revisions, UNVERIFIED for this one. The HTTP status for an unknown tool is not stated; 200 with the JSON-RPC error is used: UNVERIFIED.
//  * Not built: subscriptions/listen, MRTR input requests, resources, prompts, tasks, extensions, the legacy (2025-11-25) initialize
//    handshake. A legacy initialize request is answered -32601 with a message naming the supported version.
//
// WHAT IS EXPOSED, AND WHAT CAN NEVER BE. The tool table below is a closed allow-list of exactly five tools: get_status, list_bookmarks,
// recent_decodes (read) and tune, set_mode (the receiver's frequency and demodulator mode, nothing else). The McpHost interface that
// the wiring lane implements has exactly those five operations, so there is NO code path from this file to transmit, keying, PTT, any
// file write (recordings or anywhere else), any setting, any network listener or any plugin. A tools/call for any other name is refused
// before the host is touched; an argument that is not in a tool's schema is refused before the host is touched. The tests send
// transmit, key, record, write, settings and server-start requests under many names and prove the host saw none of them.
//
// SAFETY LIMITS: body at most 64 KiB, JSON nesting at most 32, string arguments at most 256 bytes, frequency 1 kHz to 7.2 GHz (the host
// may refuse more), mode one of the receiver's eight names exactly (core/demod_names.hpp), at most 200 decodes and 500 bookmarks per
// call, text fields of results cut to 512 bytes and cleaned to valid UTF-8, tool calls limited by a token bucket (default burst 20,
// 10 per second).
//
// THREADS: handle() may be called from several threads; the rate limiter is guarded; the host object must tolerate that.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/demod_names.hpp"

namespace cascade::net::mcp {

inline constexpr const char* kProtocolVersion = "2026-07-28";
inline constexpr const char* kMetaVersionKey = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kMetaCapsKey = "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kMetaServerInfoKey = "io.modelcontextprotocol/serverInfo";

// ---- what the host (the wiring lane) provides: exactly five operations ---------------------------------------------------------------------
struct StatusInfo {
    bool running = false;
    double centerHz = 0, sampleRateHz = 0, vfoOffsetHz = 0, bandwidthHz = 0, signalDb = 0;
    std::string mode, sourceName;
};
struct BookmarkInfo {
    std::string name, mode, group;
    double freqHz = 0;
};
struct DecodeInfo {
    double unixTime = 0, freqHz = 0;
    std::string decoder, text;
};
struct HostResult {
    bool ok = true;
    std::string error;
};

class McpHost {
public:
    virtual ~McpHost() = default;
    virtual StatusInfo status() = 0;
    virtual HostResult tune(double frequencyHz) = 0;
    virtual HostResult setMode(const std::string& modeName) = 0;
    virtual std::vector<BookmarkInfo> bookmarks() = 0;
    virtual std::vector<DecodeInfo> recentDecodes(std::size_t maxCount) = 0;
};

struct Config {
    std::string serverName = "FoxSDR";
    std::string serverVersion = "0";
    std::size_t maxBodyBytes = 64 * 1024;
    int maxNesting = 32;
    double minTuneHz = 1000.0;
    double maxTuneHz = 7.2e9;
    double burst = 20.0;
    double perSecond = 10.0;
};

struct Reply {
    int status = 200;
    bool hasBody = true;
    std::string body;                                   // JSON text (empty when !hasBody)
    std::vector<std::pair<std::string, std::string>> headers;
};

enum class Access { Read, Tune };

struct ToolDef {
    const char* name;
    const char* title;
    const char* description;
    Access access;
};

// THE ALLOW-LIST. Adding a tool is adding a row here and a case in callTool(); nothing else can ever be reached.
inline const std::vector<ToolDef>& tools() {
    static const std::vector<ToolDef> k = {
        {"get_status", "Receiver status", "Current tuning, mode, bandwidth, sample rate, source and signal level. Read only.", Access::Read},
        {"list_bookmarks", "List bookmarks", "The saved frequency bookmarks (name, frequency, mode, group). Read only.", Access::Read},
        {"recent_decodes", "Recent decodes", "The most recent decoded messages from the running decoders. Read only.", Access::Read},
        {"tune", "Tune the receiver", "Sets the receiver's centre frequency in hertz. Changes the tuning only; it cannot transmit.", Access::Tune},
        {"set_mode", "Set the demodulator mode", "Sets the demodulator mode: NFM, WFM, AM, DSB, USB, CW, LSB or RAW. It cannot transmit.", Access::Tune},
    };
    return k;
}

namespace detail {

inline std::string cleanText(const std::string& in, std::size_t maxBytes) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size() && out.size() < maxBytes) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len != 0 && i + len <= in.size();
        for (std::size_t k = 1; ok && k < len; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) ok = false;
        if (ok && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;
        }
        if (ok && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;
        }
        if (ok && len == 1 && ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F)) ok = false;
        if (!ok) { out += '?'; ++i; continue; }
        if (out.size() + len > maxBytes) break;
        out.append(in, i, len);
        i += len;
    }
    return out;
}

inline std::string dump(const nlohmann::json& j) { return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); }

// Maximum bracket nesting outside strings; a cheap pre-check before the parser sees a hostile document.
inline int nestingDepth(const std::string& s) {
    int depth = 0, maxDepth = 0;
    bool inStr = false, esc = false;
    for (char c : s) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[' || c == '{') { if (++depth > maxDepth) maxDepth = depth; }
        else if (c == ']' || c == '}') { if (depth > 0) --depth; }
    }
    return maxDepth;
}

inline bool base64Decode(const std::string& in, std::string& out) {
    static const std::string tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    unsigned val = 0;
    int bits = -8;
    for (char c : in) {
        if (c == '=') break;
        const std::size_t p = tbl.find(c);
        if (p == std::string::npos) return false;
        val = (val << 6) + static_cast<unsigned>(p);
        bits += 6;
        if (bits >= 0) { out += static_cast<char>((val >> bits) & 0xFF); bits -= 8; }
    }
    return true;
}

// "=?base64?...?=" -> decoded, anything else unchanged; false if the sentinel is malformed.
inline bool decodeHeaderValue(const std::string& v, std::string& out) {
    if (v.size() >= 11 && v.rfind("=?base64?", 0) == 0 && v.compare(v.size() - 2, 2, "?=") == 0)
        return base64Decode(v.substr(9, v.size() - 11), out);
    out = v;
    return true;
}

}  // namespace detail

class McpServer {
public:
    McpServer(McpHost& host, Config cfg = Config{}) : host_(host), cfg_(std::move(cfg)), tokens_(cfg_.burst) {
        clock_ = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        last_ = clock_();
    }

    // A clock in seconds, for the rate limiter (tests).
    void setClock(std::function<double()> c) {
        std::lock_guard<std::mutex> lk(rl_);
        clock_ = std::move(c);
        last_ = clock_();
        tokens_ = cfg_.burst;
    }

    const Config& config() const { return cfg_; }

    // One JSON-RPC message in, one reply out. `headerCheck`, when set, is given the parsed request after the envelope checks and may
    // return an HTTP-level rejection (the transport's header rules); it is not called for notifications.
    using HeaderCheck = std::function<std::optional<Reply>(const nlohmann::json& message)>;

    Reply handle(const std::string& body, const HeaderCheck& headerCheck = nullptr) {
        using nlohmann::json;
        if (body.size() > cfg_.maxBodyBytes) return error(413, json(nullptr), -32600, "request too large");
        if (detail::nestingDepth(body) > cfg_.maxNesting) return error(400, json(nullptr), -32600, "message nested too deeply");
        json msg = json::parse(body, nullptr, false);
        if (msg.is_discarded()) return error(400, json(nullptr), -32700, "parse error");
        if (msg.is_array()) return error(400, json(nullptr), -32600, "batch messages are not supported");
        if (!msg.is_object()) return error(400, json(nullptr), -32600, "invalid request");
        const bool hasId = msg.contains("id");
        json id = nullptr;
        if (hasId) {
            const json& i = msg["id"];
            if (!(i.is_string() || i.is_number_integer())) return error(400, json(nullptr), -32600, "the id must be a string or an integer");
            id = i;
        }
        if (!msg.contains("jsonrpc") || msg["jsonrpc"] != "2.0") return error(400, id, -32600, "jsonrpc must be \"2.0\"");
        if (!msg.contains("method") || !msg["method"].is_string()) return error(400, id, -32600, "method must be a string");
        const std::string method = msg["method"].get<std::string>();
        if (!hasId) {
            Reply r;
            r.status = 202;
            r.hasBody = false;
            return r;
        }
        if (method != "server/discover" && method != "tools/list" && method != "tools/call") {
            if (method == "initialize")
                return error(404, id, -32601, std::string("method not found: this server speaks protocol version ") + kProtocolVersion + " only (no initialize handshake)");
            return error(404, id, -32601, "method not found");
        }
        json params = json::object();
        if (msg.contains("params")) {
            if (!msg["params"].is_object()) return error(400, id, -32602, "params must be an object");
            params = msg["params"];
        }
        if (!params.contains("_meta") || !params["_meta"].is_object()) return error(400, id, -32602, "params._meta is required");
        const json& meta = params["_meta"];
        if (!meta.contains(kMetaVersionKey) || !meta[kMetaVersionKey].is_string()) return error(400, id, -32602, "protocolVersion is required in _meta");
        if (!meta.contains(kMetaCapsKey) || !meta[kMetaCapsKey].is_object()) return error(400, id, -32602, "clientCapabilities is required in _meta");
        if (meta[kMetaVersionKey].get<std::string>() != kProtocolVersion) {
            json data;
            data["supported"] = json::array({kProtocolVersion});
            data["requested"] = detail::cleanText(meta[kMetaVersionKey].get<std::string>(), 64);
            return error(400, id, -32022, "Unsupported protocol version", &data);
        }
        if (headerCheck) {
            if (auto rej = headerCheck(msg)) return *rej;
        }
        if (method == "server/discover") return discover(id);
        if (method == "tools/list") return listTools(id);
        return callTool(id, params);
    }

    static std::string errorJson(const nlohmann::json& id, int code, const std::string& message, const nlohmann::json* data = nullptr) {
        nlohmann::json j;
        j["jsonrpc"] = "2.0";
        j["id"] = id;
        j["error"] = {{"code", code}, {"message", message}};
        if (data) j["error"]["data"] = *data;
        return detail::dump(j);
    }

private:
    Reply error(int status, const nlohmann::json& id, int code, const std::string& message, const nlohmann::json* data = nullptr) const {
        Reply r;
        r.status = status;
        r.body = errorJson(id, code, message, data);
        return r;
    }

    nlohmann::json serverInfo() const { return {{"name", cfg_.serverName}, {"version", cfg_.serverVersion}}; }

    Reply result(const nlohmann::json& id, nlohmann::json res) const {
        res["resultType"] = "complete";
        res["_meta"][kMetaServerInfoKey] = serverInfo();
        nlohmann::json j;
        j["jsonrpc"] = "2.0";
        j["id"] = id;
        j["result"] = std::move(res);
        Reply r;
        r.body = detail::dump(j);
        return r;
    }

    Reply discover(const nlohmann::json& id) const {
        nlohmann::json res;
        res["supportedVersions"] = nlohmann::json::array({kProtocolVersion});
        res["capabilities"] = {{"tools", {{"listChanged", false}}}};
        res["instructions"] =
            "Read the receiver's status, bookmarks and recent decodes, and tune it or change its demodulator mode. "
            "This server cannot transmit, record, write files or change settings.";
        return result(id, std::move(res));
    }

    static nlohmann::json schemaFor(const std::string& name, const Config& cfg) {
        using nlohmann::json;
        if (name == "get_status") return {{"type", "object"}, {"additionalProperties", false}};
        if (name == "list_bookmarks")
            return {{"type", "object"}, {"additionalProperties", false},
                    {"properties", {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"default", 100}}},
                                    {"group", {{"type", "string"}, {"maxLength", 256}}}}}};
        if (name == "recent_decodes")
            return {{"type", "object"}, {"additionalProperties", false},
                    {"properties", {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"default", 20}}},
                                    {"decoder", {{"type", "string"}, {"maxLength", 256}}}}}};
        if (name == "tune")
            return {{"type", "object"}, {"additionalProperties", false}, {"required", json::array({"frequency_hz"})},
                    {"properties", {{"frequency_hz", {{"type", "number"}, {"minimum", cfg.minTuneHz}, {"maximum", cfg.maxTuneHz}}}}}};
        json modes = json::array();
        for (int i = 1; i <= 8; ++i) modes.push_back(core::kDemodNames[i]);
        return {{"type", "object"}, {"additionalProperties", false}, {"required", json::array({"mode"})},
                {"properties", {{"mode", {{"type", "string"}, {"enum", modes}}}}}};
    }

    Reply listTools(const nlohmann::json& id) const {
        nlohmann::json arr = nlohmann::json::array();
        for (const ToolDef& t : tools()) {
            nlohmann::json o;
            o["name"] = t.name;
            o["title"] = t.title;
            o["description"] = t.description;
            o["inputSchema"] = schemaFor(t.name, cfg_);
            o["annotations"] = {{"readOnlyHint", t.access == Access::Read}, {"destructiveHint", false},
                                {"idempotentHint", true}, {"openWorldHint", false}};
            arr.push_back(std::move(o));
        }
        nlohmann::json res;
        res["tools"] = std::move(arr);
        return result(id, std::move(res));
    }

    bool takeToken() {
        std::lock_guard<std::mutex> lk(rl_);
        const double now = clock_();
        const double dt = now - last_;
        last_ = now;
        if (dt > 0) tokens_ = std::min(cfg_.burst, tokens_ + dt * cfg_.perSecond);
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

    Reply toolResult(const nlohmann::json& id, const nlohmann::json& structured, bool isError) const {
        nlohmann::json res;
        res["content"] = nlohmann::json::array({{{"type", "text"}, {"text", detail::dump(structured)}}});
        if (!isError) res["structuredContent"] = structured;
        res["isError"] = isError;
        return result(id, std::move(res));
    }
    Reply toolError(const nlohmann::json& id, const std::string& message) const {
        return toolResult(id, {{"error", detail::cleanText(message, 512)}}, true);
    }

    // Validates a property map against an allow-list of names: an unknown property is refused.
    static std::optional<std::string> unknownArg(const nlohmann::json& args, std::initializer_list<const char*> allowed) {
        for (auto it = args.begin(); it != args.end(); ++it) {
            bool ok = false;
            for (const char* a : allowed)
                if (it.key() == a) ok = true;
            if (!ok) return "unexpected argument '" + detail::cleanText(it.key(), 64) + "'";
        }
        return std::nullopt;
    }

    Reply callTool(const nlohmann::json& id, const nlohmann::json& params) {
        using nlohmann::json;
        if (!params.contains("name") || !params["name"].is_string()) return error(400, id, -32602, "name must be a string");
        const std::string name = params["name"].get<std::string>();
        json args = json::object();
        if (params.contains("arguments")) {
            if (!params["arguments"].is_object()) return error(400, id, -32602, "arguments must be an object");
            args = params["arguments"];
        }
        const ToolDef* def = nullptr;
        for (const ToolDef& t : tools())
            if (name == t.name) def = &t;
        if (!def) return error(200, id, -32602, "Unknown tool: " + detail::cleanText(name, 64));   // the host is never touched
        if (!takeToken()) return toolError(id, "rate limit exceeded: too many tool calls, retry shortly");
        try {
            if (name == "get_status") {
                if (auto u = unknownArg(args, {})) return toolError(id, *u);
                const StatusInfo s = host_.status();
                return toolResult(id, {{"running", s.running}, {"center_hz", finite(s.centerHz)}, {"sample_rate_hz", finite(s.sampleRateHz)},
                                       {"vfo_offset_hz", finite(s.vfoOffsetHz)}, {"bandwidth_hz", finite(s.bandwidthHz)},
                                       {"mode", detail::cleanText(s.mode, 32)}, {"source", detail::cleanText(s.sourceName, 128)},
                                       {"signal_db", finite(s.signalDb)}}, false);
            }
            if (name == "list_bookmarks") {
                if (auto u = unknownArg(args, {"limit", "group"})) return toolError(id, *u);
                std::size_t limit = 100;
                if (args.contains("limit")) {
                    if (!args["limit"].is_number_integer()) return toolError(id, "limit must be an integer");
                    const long long v = args["limit"].get<long long>();
                    if (v < 1 || v > 500) return toolError(id, "limit must be between 1 and 500");
                    limit = static_cast<std::size_t>(v);
                }
                std::string group;
                const bool filter = args.contains("group");
                if (filter) {
                    if (!args["group"].is_string()) return toolError(id, "group must be a string");
                    group = args["group"].get<std::string>();
                    if (group.size() > 256) return toolError(id, "group is too long");
                }
                json list = json::array();
                for (const BookmarkInfo& b : host_.bookmarks()) {
                    if (filter && b.group != group) continue;
                    if (list.size() >= limit) break;
                    list.push_back({{"name", detail::cleanText(b.name, 128)}, {"frequency_hz", finite(b.freqHz)},
                                    {"mode", detail::cleanText(b.mode, 32)}, {"group", detail::cleanText(b.group, 128)}});
                }
                return toolResult(id, {{"bookmarks", list}}, false);
            }
            if (name == "recent_decodes") {
                if (auto u = unknownArg(args, {"limit", "decoder"})) return toolError(id, *u);
                std::size_t limit = 20;
                if (args.contains("limit")) {
                    if (!args["limit"].is_number_integer()) return toolError(id, "limit must be an integer");
                    const long long v = args["limit"].get<long long>();
                    if (v < 1 || v > 200) return toolError(id, "limit must be between 1 and 200");
                    limit = static_cast<std::size_t>(v);
                }
                std::string decoder;
                const bool filter = args.contains("decoder");
                if (filter) {
                    if (!args["decoder"].is_string()) return toolError(id, "decoder must be a string");
                    decoder = args["decoder"].get<std::string>();
                    if (decoder.size() > 256) return toolError(id, "decoder is too long");
                }
                json list = json::array();
                for (const DecodeInfo& d : host_.recentDecodes(limit)) {
                    if (filter && d.decoder != decoder) continue;
                    if (list.size() >= limit) break;
                    list.push_back({{"time", finite(d.unixTime)}, {"decoder", detail::cleanText(d.decoder, 64)},
                                    {"frequency_hz", finite(d.freqHz)}, {"text", detail::cleanText(d.text, 512)}});
                }
                return toolResult(id, {{"decodes", list}}, false);
            }
            if (name == "tune") {
                if (auto u = unknownArg(args, {"frequency_hz"})) return toolError(id, *u);
                if (!args.contains("frequency_hz") || !args["frequency_hz"].is_number()) return toolError(id, "frequency_hz is required and must be a number");
                const double f = args["frequency_hz"].get<double>();
                if (!std::isfinite(f) || f < cfg_.minTuneHz || f > cfg_.maxTuneHz) return toolError(id, "frequency_hz is outside the allowed range");
                const HostResult r = host_.tune(f);
                if (!r.ok) return toolError(id, r.error.empty() ? "the receiver refused the frequency" : r.error);
                return toolResult(id, {{"tuned", true}, {"frequency_hz", f}}, false);
            }
            if (name == "set_mode") {
                if (auto u = unknownArg(args, {"mode"})) return toolError(id, *u);
                if (!args.contains("mode") || !args["mode"].is_string()) return toolError(id, "mode is required and must be a string");
                const std::string m = args["mode"].get<std::string>();
                bool known = false;
                for (int i = 1; i <= 8; ++i)
                    if (m == core::kDemodNames[i]) known = true;
                if (!known) return toolError(id, "mode must be one of NFM, WFM, AM, DSB, USB, CW, LSB, RAW");
                const HostResult r = host_.setMode(m);
                if (!r.ok) return toolError(id, r.error.empty() ? "the receiver refused the mode" : r.error);
                return toolResult(id, {{"mode", m}}, false);
            }
        } catch (const std::exception&) {
            return toolError(id, "internal error while running the tool");
        } catch (...) {
            return toolError(id, "internal error while running the tool");
        }
        return error(200, id, -32602, "Unknown tool");
    }

    static nlohmann::json finite(double v) { return std::isfinite(v) ? nlohmann::json(v) : nlohmann::json(nullptr); }

    McpHost& host_;
    Config cfg_;
    std::mutex rl_;
    std::function<double()> clock_;
    double tokens_, last_ = 0;
};

// ---- the Streamable HTTP request policy ------------------------------------------------------------------------------------------------------
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;   // names LOWER-CASED by the caller
    std::string body;
};

struct HttpPolicyConfig {
    bool enabled = false;               // OFF by default: a disabled endpoint answers 404 to everything
    std::string path = "/mcp";
    std::string bearerToken;            // when set, "Authorization: Bearer <token>" is required (the spec says servers SHOULD authenticate)
};

namespace detail {

inline std::string lowerStr(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// http://localhost[:port], http://127.x.y.z[:port], http://[::1][:port] (and https of the same). A missing scheme, any other host, userinfo or
// a path are refused.
inline bool loopbackOrigin(const std::string& origin) {
    std::string o = lowerStr(origin);
    std::size_t p;
    if (o.rfind("http://", 0) == 0) p = 7;
    else if (o.rfind("https://", 0) == 0) p = 8;
    else return false;
    std::string hostport = o.substr(p);
    if (hostport.empty() || hostport.find_first_of("/@?# \t") != std::string::npos) return false;
    std::string host = hostport, port;
    if (hostport[0] == '[') {
        const std::size_t c = hostport.find(']');
        if (c == std::string::npos) return false;
        host = hostport.substr(0, c + 1);
        if (c + 1 < hostport.size()) { if (hostport[c + 1] != ':') return false; port = hostport.substr(c + 2); }
    } else {
        const std::size_t c = hostport.find(':');
        if (c != std::string::npos) { host = hostport.substr(0, c); port = hostport.substr(c + 1); }
    }
    if (!port.empty()) {
        if (port.size() > 5) return false;
        for (char ch : port)
            if (ch < '0' || ch > '9') return false;
    } else if (hostport.find(':') != std::string::npos && hostport[0] != '[') {
        return false;   // "host:" with an empty port
    }
    if (host == "localhost" || host == "[::1]") return true;
    // 127.0.0.0/8 dotted quad
    int parts = 0, val = -1, first = -1;
    int digits = 0;
    for (char ch : host + ".") {
        if (ch == '.') {
            if (digits == 0 || val > 255) return false;
            if (parts == 0) first = val;
            ++parts;
            val = -1;
            digits = 0;
        } else if (ch >= '0' && ch <= '9') {
            val = (val < 0 ? 0 : val) * 10 + (ch - '0');
            ++digits;
            if (digits > 3) return false;
        } else {
            return false;
        }
    }
    return parts == 4 && first == 127;
}

inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

}  // namespace detail

class McpHttpPolicy {
public:
    McpHttpPolicy(McpServer& server, HttpPolicyConfig cfg = HttpPolicyConfig{}) : server_(server), cfg_(std::move(cfg)) {}

    Reply handle(const HttpRequest& req) {
        using nlohmann::json;
        auto plain = [](int status, const std::string& text) {
            Reply r;
            r.status = status;
            r.body = text;
            r.headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
            return r;
        };
        auto rpcError = [&](int status, int code, const std::string& msg) {
            Reply r;
            r.status = status;
            r.body = McpServer::errorJson(json(nullptr), code, msg);
            return r;
        };
        auto header = [&](const char* name) -> const std::string* {
            auto it = req.headers.find(name);
            return it == req.headers.end() ? nullptr : &it->second;
        };
        auto finish = [](Reply r) {
            bool haveType = false;
            for (const auto& h : r.headers) if (detail::lowerStr(h.first) == "content-type") haveType = true;
            if (!haveType && r.hasBody) r.headers.push_back({"Content-Type", "application/json"});
            r.headers.push_back({"Cache-Control", "no-store"});
            r.headers.push_back({"X-Content-Type-Options", "nosniff"});
            return r;
        };
        if (!cfg_.enabled || req.path.substr(0, req.path.find('?')) != cfg_.path) return finish(plain(404, "not found"));
        // DNS rebinding: the Host and the Origin must both be loopback
        if (const std::string* host = header("host")) {
            if (!detail::loopbackOrigin("http://" + *host)) return finish(rpcError(403, -32600, "forbidden host"));
        } else {
            return finish(rpcError(403, -32600, "forbidden host"));
        }
        if (const std::string* origin = header("origin")) {
            if (!detail::loopbackOrigin(*origin)) return finish(rpcError(403, -32600, "forbidden origin"));
        }
        if (!cfg_.bearerToken.empty()) {
            bool ok = false;
            if (const std::string* a = header("authorization")) {
                static const std::string scheme = "bearer ";
                if (a->size() > scheme.size() && a->size() < 4096 && detail::lowerStr(a->substr(0, scheme.size())) == scheme)
                    ok = detail::constantTimeEquals(a->substr(scheme.size()), cfg_.bearerToken);
            }
            if (!ok) {
                Reply r = plain(401, "unauthorized");
                r.headers.push_back({"WWW-Authenticate", "Bearer"});
                return finish(std::move(r));
            }
        }
        if (req.method != "POST") {
            Reply r = plain(405, "method not allowed");
            r.headers.push_back({"Allow", "POST"});
            return finish(std::move(r));
        }
        if (req.body.size() > server_.config().maxBodyBytes) return finish(rpcError(413, -32600, "request too large"));
        const std::string* ct = header("content-type");
        if (!ct || detail::lowerStr(ct->substr(0, ct->find(';'))) != "application/json") return finish(plain(415, "content type must be application/json"));
        const std::string* accept = header("accept");
        if (accept) {
            const std::string a = detail::lowerStr(*accept);
            if (a.find("application/json") == std::string::npos && a.find("*/*") == std::string::npos && a.find("application/*") == std::string::npos)
                return finish(plain(406, "Accept must allow application/json"));
        }
        Reply r = server_.handle(req.body, [&](const json& msg) -> std::optional<Reply> {
            auto mismatch = [&](const std::string& what) {
                Reply m;
                m.status = 400;
                m.body = McpServer::errorJson(msg.contains("id") ? msg["id"] : json(nullptr), -32020, "Header mismatch: " + what);
                return std::optional<Reply>(m);
            };
            const std::string* ver = header("mcp-protocol-version");
            if (!ver) return mismatch("MCP-Protocol-Version header is missing");
            if (*ver != msg["params"]["_meta"][kMetaVersionKey].get<std::string>()) return mismatch("MCP-Protocol-Version does not match the request body");
            const std::string* method = header("mcp-method");
            if (!method) return mismatch("Mcp-Method header is missing");
            if (*method != msg["method"].get<std::string>()) return mismatch("Mcp-Method does not match the request body");
            if (msg["method"] == "tools/call") {
                const std::string* nm = header("mcp-name");
                if (!nm) return mismatch("Mcp-Name header is missing");
                std::string decoded;
                if (!detail::decodeHeaderValue(*nm, decoded)) return mismatch("Mcp-Name is not valid base64");
                if (!msg["params"].contains("name") || !msg["params"]["name"].is_string() || decoded != msg["params"]["name"].get<std::string>())
                    return mismatch("Mcp-Name does not match the request body");
            }
            return std::nullopt;
        });
        return finish(std::move(r));
    }

private:
    McpServer& server_;
    HttpPolicyConfig cfg_;
};

}  // namespace cascade::net::mcp
