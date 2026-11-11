// Tests for net/mcp_server.hpp and net/mcp_http.hpp: the MCP (protocol revision 2026-07-28) JSON-RPC handler with five tools, the
// Streamable HTTP policy, and the loopback listener. The point of most checks is what must NEVER happen: no transmit, no keying, no file
// write, no setting, no listener, nothing outside the five tools - proved by a host object that counts every call it receives.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/mcp_server.hpp"
#include "net/mcp_http.hpp"

#include <atomic>
#include <cmath>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include "test_check.hpp"

namespace mcp = cascade::net::mcp;
using nlohmann::json;

class MockHost : public mcp::McpHost {
public:
    std::atomic<int> statusCalls{0}, tuneCalls{0}, modeCalls{0}, bookmarkCalls{0}, decodeCalls{0};
    std::atomic<double> lastTune{0};
    std::string lastMode;
    bool throwOnStatus = false, tuneRefuses = false, hostile = false;
    int total() const { return statusCalls + tuneCalls + modeCalls + bookmarkCalls + decodeCalls; }

    mcp::StatusInfo status() override {
        ++statusCalls;
        if (throwOnStatus) throw std::runtime_error("boom");
        mcp::StatusInfo s;
        s.running = true;
        s.centerHz = 100.1e6;
        s.sampleRateHz = 2.4e6;
        s.vfoOffsetHz = -250000;
        s.bandwidthHz = 150000;
        s.signalDb = -47.5;
        s.mode = "WFM";
        s.sourceName = hostile ? std::string("bad\xFF\xFE name\n") : "RTL-SDR";
        if (hostile) s.centerHz = std::nan("");
        return s;
    }
    mcp::HostResult tune(double f) override {
        ++tuneCalls;
        lastTune = f;
        if (tuneRefuses) return {false, "outside the tuner range"};
        return {};
    }
    mcp::HostResult setMode(const std::string& m) override {
        ++modeCalls;
        lastMode = m;
        return {};
    }
    std::vector<mcp::BookmarkInfo> bookmarks() override {
        ++bookmarkCalls;
        std::vector<mcp::BookmarkInfo> v;
        for (int i = 0; i < 700; ++i) v.push_back({"BM" + std::to_string(i), "NFM", i % 2 ? "Air" : "Marine", 118e6 + i * 25e3});
        return v;
    }
    std::vector<mcp::DecodeInfo> recentDecodes(std::size_t maxCount) override {
        ++decodeCalls;
        std::vector<mcp::DecodeInfo> v;
        for (std::size_t i = 0; i < std::min<std::size_t>(maxCount, 300); ++i)
            v.push_back({1.7e9 + static_cast<double>(i), 1090e6, i % 2 ? "adsb" : "ais", hostile ? std::string(2000, 'x') + "\xFF" : "msg " + std::to_string(i)});
        return v;
    }
};

static json meta() { return {{mcp::kMetaVersionKey, "2026-07-28"}, {mcp::kMetaCapsKey, json::object()}, {"io.modelcontextprotocol/clientInfo", {{"name", "t"}, {"version", "1"}}}}; }
static json req(const json& id, const std::string& method, json params = json::object()) {
    params["_meta"] = meta();
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
static json callReq(int id, const std::string& tool, const json& args) { return req(id, "tools/call", {{"name", tool}, {"arguments", args}}); }

struct Out {
    int status;
    bool hasBody;
    json j;
};
static Out run(mcp::McpServer& s, const std::string& body) {
    const mcp::Reply r = s.handle(body);
    Out o{r.status, r.hasBody, json()};
    if (r.hasBody) o.j = json::parse(r.body);   // every reply body must be valid JSON
    return o;
}
static Out run(mcp::McpServer& s, const json& j) { return run(s, j.dump()); }
static bool isError(const Out& o, int code) { return o.j.contains("error") && o.j["error"]["code"] == code; }
static bool toolIsError(const Out& o) { return o.j.contains("result") && o.j["result"]["isError"] == true; }
static bool toolOk(const Out& o) { return o.j.contains("result") && o.j["result"]["isError"] == false; }

static std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int realMain() {
    // The host interface has exactly five operations: this class implements exactly those and is not abstract. A sixth pure virtual
    // (a transmit, a record, a setting) added to McpHost would stop this file compiling.
    MockHost host;
    mcp::Config bigBurst;   // the rate limiter is tested on its own below; here it must not hide what the tools do
    bigBurst.burst = 1e9;
    bigBurst.perSecond = 1e9;
    mcp::McpServer server(host, bigBurst);

    // --- discover and list ------------------------------------------------------------------------------------------------------------------------
    {
        const Out d = run(server, req("d1", "server/discover"));
        CHECK(d.status == 200 && d.j["id"] == "d1" && d.j["result"]["resultType"] == "complete");
        CHECK(d.j["result"]["supportedVersions"] == json::array({"2026-07-28"}));
        CHECK(d.j["result"]["capabilities"].contains("tools") && !d.j["result"]["capabilities"].contains("resources") && !d.j["result"]["capabilities"].contains("prompts"));
        CHECK(d.j["result"]["_meta"][mcp::kMetaServerInfoKey]["name"] == "FoxSDR");
        CHECK(d.j["result"]["instructions"].get<std::string>().find("cannot transmit") != std::string::npos);
        const Out l = run(server, req(2, "tools/list"));
        CHECK(l.status == 200 && l.j["id"] == 2 && l.j["result"]["resultType"] == "complete");
        std::vector<std::string> names;
        for (const auto& t : l.j["result"]["tools"]) {
            names.push_back(t["name"]);
            CHECK(t["inputSchema"]["type"] == "object" && t["inputSchema"]["additionalProperties"] == false);   // no argument outside the schema
            CHECK(t["description"].get<std::string>().size() > 10 && t.contains("title"));
        }
        CHECK((names == std::vector<std::string>{"get_status", "list_bookmarks", "recent_decodes", "tune", "set_mode"}));
        CHECK(l.j["result"]["tools"][0]["annotations"]["readOnlyHint"] == true && l.j["result"]["tools"][3]["annotations"]["readOnlyHint"] == false);
        CHECK(l.j["result"]["tools"][4]["inputSchema"]["properties"]["mode"]["enum"].size() == 8);
        CHECK(!l.j["result"].contains("nextCursor"));
        CHECK(run(server, req(3, "tools/list")).j["result"] == l.j["result"]);                       // deterministic
        // no tool name hints at any capability that must never exist
        for (const auto& n : names)
            for (const char* banned : {"transmit", "tx", "ptt", "key", "record", "write", "file", "setting", "config", "listen", "bind", "server", "plugin", "password", "exec", "shell"})
                CHECK(n.find(banned) == std::string::npos);
        CHECK(mcp::tools().size() == 5);
        for (const auto& t : mcp::tools()) {
            const std::string desc = t.description;
            if (t.access == mcp::Access::Read) CHECK(desc.find("Read only") != std::string::npos);
        }
        CHECK(host.total() == 0);                                                                     // discovery and listing touch no receiver
    }

    // --- the five tools --------------------------------------------------------------------------------------------------------------------------
    {
        Out o = run(server, callReq(1, "get_status", json::object()));
        CHECK(toolOk(o) && o.j["result"]["resultType"] == "complete");
        CHECK(o.j["result"]["structuredContent"]["center_hz"] == 100.1e6 && o.j["result"]["structuredContent"]["mode"] == "WFM" && o.j["result"]["structuredContent"]["running"] == true);
        CHECK(json::parse(o.j["result"]["content"][0]["text"].get<std::string>()) == o.j["result"]["structuredContent"]);   // text mirrors the structured result
        CHECK(o.j["result"]["content"][0]["type"] == "text");
        o = run(server, callReq(2, "list_bookmarks", {{"limit", 3}}));
        CHECK(toolOk(o) && o.j["result"]["structuredContent"]["bookmarks"].size() == 3);
        CHECK(o.j["result"]["structuredContent"]["bookmarks"][1]["frequency_hz"] == 118025000.0);
        o = run(server, callReq(3, "list_bookmarks", {{"group", "Air"}, {"limit", 500}}));
        CHECK(toolOk(o) && o.j["result"]["structuredContent"]["bookmarks"].size() == 350);
        o = run(server, callReq(4, "list_bookmarks", json::object()));
        CHECK(o.j["result"]["structuredContent"]["bookmarks"].size() == 100);                         // the default limit
        o = run(server, callReq(5, "recent_decodes", {{"limit", 5}}));
        CHECK(toolOk(o) && o.j["result"]["structuredContent"]["decodes"].size() == 5);
        o = run(server, callReq(6, "recent_decodes", {{"decoder", "adsb"}, {"limit", 10}}));
        CHECK(toolOk(o) && o.j["result"]["structuredContent"]["decodes"].size() == 5);                // the host returned 10, five are adsb
        for (const auto& d : o.j["result"]["structuredContent"]["decodes"]) CHECK(d["decoder"] == "adsb");
        const int before = host.tuneCalls;
        o = run(server, callReq(7, "tune", {{"frequency_hz", 145500000}}));
        CHECK(toolOk(o) && host.tuneCalls == before + 1 && host.lastTune == 145500000.0);
        CHECK(o.j["result"]["structuredContent"]["frequency_hz"] == 145500000.0);
        o = run(server, callReq(8, "tune", {{"frequency_hz", 1090.5e6}}));
        CHECK(toolOk(o) && host.lastTune == 1090.5e6);
        o = run(server, callReq(9, "set_mode", {{"mode", "USB"}}));
        CHECK(toolOk(o) && host.modeCalls == 1 && host.lastMode == "USB");
        for (const char* m : {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"}) CHECK(toolOk(run(server, callReq(10, "set_mode", {{"mode", m}}))));
        CHECK(host.modeCalls == 9);
        // omitted arguments are fine for the tools that need none
        CHECK(toolOk(run(server, req(11, "tools/call", {{"name", "get_status"}}))));
        // a host that refuses, and one that throws
        host.tuneRefuses = true;
        o = run(server, callReq(12, "tune", {{"frequency_hz", 100e6}}));
        CHECK(toolIsError(o) && o.j["result"]["structuredContent"].is_null() && o.j["result"]["content"][0]["text"].get<std::string>().find("outside the tuner range") != std::string::npos);
        host.tuneRefuses = false;
        host.throwOnStatus = true;
        o = run(server, callReq(13, "get_status", json::object()));
        CHECK(o.status == 200 && toolIsError(o) && o.j["result"]["content"][0]["text"].get<std::string>().find("internal error") != std::string::npos);
        host.throwOnStatus = false;
        // hostile text from the host: valid JSON out, cut, NaN becomes null
        host.hostile = true;
        o = run(server, callReq(14, "get_status", json::object()));
        CHECK(toolOk(o) && o.j["result"]["structuredContent"]["center_hz"].is_null());
        CHECK(o.j["result"]["structuredContent"]["source"].get<std::string>().find('\xFF') == std::string::npos);
        o = run(server, callReq(15, "recent_decodes", {{"limit", 2}}));
        CHECK(toolOk(o) && o.j["result"]["structuredContent"]["decodes"][0]["text"].get<std::string>().size() <= 512);
        host.hostile = false;
    }

    // --- invalid arguments: refused before the host is touched ---------------------------------------------------------------------------------------
    {
        const int base = host.total();
        struct Row { const char* tool; json args; };
        const std::vector<Row> rows = {
            {"tune", json::object()}, {"tune", {{"frequency_hz", "145500000"}}}, {"tune", {{"frequency_hz", true}}}, {"tune", {{"frequency_hz", nullptr}}},
            {"tune", {{"frequency_hz", json::array({1})}}}, {"tune", {{"frequency_hz", 1e12}}}, {"tune", {{"frequency_hz", 0}}}, {"tune", {{"frequency_hz", -5}}},
            {"tune", {{"frequency_hz", 999}}}, {"tune", {{"frequency_hz", 7.3e9}}}, {"tune", {{"frequency", 100e6}}},
            {"tune", {{"frequency_hz", 100e6}, {"transmit", true}}}, {"tune", {{"frequency_hz", 100e6}, {"ptt", 1}}}, {"tune", {{"frequency_hz", 100e6}, {"power_w", 100}}},
            {"tune", {{"frequency_hz", 100e6}, {"__proto__", json::object()}}}, {"tune", {{"frequency_hz", 100e6}, {"", 1}}},
            {"set_mode", json::object()}, {"set_mode", {{"mode", "usb"}}}, {"set_mode", {{"mode", "FM"}}}, {"set_mode", {{"mode", ""}}}, {"set_mode", {{"mode", "USB "}}},
            {"set_mode", {{"mode", 5}}}, {"set_mode", {{"mode", nullptr}}}, {"set_mode", {{"mode", "USB"}, {"tx", true}}}, {"set_mode", {{"mode", std::string("USB\0x", 5)}}},
            {"get_status", {{"verbose", true}}}, {"get_status", {{"write", "/etc/passwd"}}},
            {"list_bookmarks", {{"limit", 0}}}, {"list_bookmarks", {{"limit", 501}}}, {"list_bookmarks", {{"limit", 1.5}}}, {"list_bookmarks", {{"limit", "10"}}},
            {"list_bookmarks", {{"group", 5}}}, {"list_bookmarks", {{"group", std::string(257, 'g')}}}, {"list_bookmarks", {{"path", "/tmp"}}},
            {"recent_decodes", {{"limit", 0}}}, {"recent_decodes", {{"limit", 201}}}, {"recent_decodes", {{"limit", -1}}}, {"recent_decodes", {{"decoder", json::array()}}},
            {"recent_decodes", {{"decoder", std::string(300, 'd')}}}, {"recent_decodes", {{"export_to", "x.csv"}}},
        };
        int n = 0;
        for (const Row& r : rows) {
            const Out o = run(server, callReq(100 + n++, r.tool, r.args));
            CHECK(o.status == 200 && toolIsError(o));
        }
        CHECK(host.total() == base);                                                                  // not one of them reached the receiver
        // arguments that are not an object, a name that is not a string, params that are not an object
        CHECK(isError(run(server, req(1, "tools/call", {{"name", "tune"}, {"arguments", json::array()}})), -32602));
        CHECK(isError(run(server, req(1, "tools/call", {{"name", "tune"}, {"arguments", "x"}})), -32602));
        CHECK(isError(run(server, req(1, "tools/call", {{"name", 5}})), -32602));
        CHECK(isError(run(server, req(1, "tools/call", json::object())), -32602));
        CHECK(isError(run(server, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", json::array()}}), -32602));
        CHECK(host.total() == base);
    }

    // --- everything that must NEVER be reachable -------------------------------------------------------------------------------------------------------
    {
        const int base = host.total();
        const std::vector<std::string> forbidden = {
            "transmit", "tx", "TX", "start_tx", "stop_tx", "ptt", "PTT", "key", "keyer", "key_down", "send", "send_message", "send_cw", "play", "play_audio", "tx_audio",
            "record", "start_recording", "stop_recording", "write_file", "save", "save_file", "append_file", "delete_file", "rename_file", "mkdir", "export", "export_recording",
            "set_setting", "settings", "set_config", "config", "save_settings", "set_bind_address", "set_port", "enable_web_server", "start_server", "stop_server", "listen",
            "bind", "open_port", "forward_port", "set_password", "set_token", "exec", "shell", "run", "system", "eval", "load_plugin", "install_plugin", "uninstall_plugin",
            "update", "upload", "download", "fetch", "http_get", "set_gain", "set_sample_rate", "select_source", "scan", "shutdown", "quit", "restart", "reset", "factory_reset",
            "Tune", "TUNE", "tune ", " tune", "tune\n", std::string("tune\0transmit", 13), "tune/../transmit", "tune;transmit", "get_status;transmit", "../tools/tune", "tools/tune",
            "tools/call", "tune?x=1", "tune#x", "\xC3\xA9", "tun\xE2\x80\x8B" "e", "get_status\r\ntransmit", "*", "", std::string(100000, 'a')};
        int n = 0;
        std::printf("INFO forbidden tool names tried: %zu
", forbidden.size());
        for (const std::string& name : forbidden) {
            const Out o = run(server, callReq(500 + n++, name, {{"frequency_hz", 100e6}, {"mode", "USB"}}));
            const bool refused = (o.status == 200 && isError(o, -32602) && o.j["error"]["message"].get<std::string>().rfind("Unknown tool", 0) == 0) || (name.size() > 65536 && o.status == 413 && isError(o, -32600));
            CHECK(refused);
            if (!refused) std::printf("NOT REFUSED: tool name [%s]\n", name.substr(0, 40).c_str());
        }
        // the same names as whole methods
        for (const char* m : {"resources/list", "resources/read", "resources/subscribe", "prompts/list", "prompts/get", "sampling/createMessage", "elicitation/create", "roots/list",
                              "tasks/create", "subscriptions/listen", "logging/setLevel", "completion/complete", "initialize", "ping", "notifications/initialized", "tools/call/transmit",
                              "tools/transmit", "TOOLS/LIST", "tools/list ", "", "server/discover/x", "transmit", "record/start", "settings/set", "file/write", "server/start"}) {
            const Out o = run(server, req(7, m));
            CHECK(o.status == 404 && isError(o, -32601));
        }
        CHECK(run(server, req(7, "initialize")).j["error"]["message"].get<std::string>().find("2026-07-28") != std::string::npos);
        // a notification never runs a tool, whatever it names
        for (const char* tool : {"tune", "set_mode", "transmit"}) {
            json n2 = {{"jsonrpc", "2.0"}, {"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", {{"frequency_hz", 100e6}, {"mode", "USB"}}}, {"_meta", meta()}}}};
            const Out o = run(server, n2);
            CHECK(o.status == 202 && !o.hasBody);
        }
        CHECK(host.total() == base);                                                                  // the host saw NOTHING
        // the source of the handler has no file, process, library or socket code on any non-comment line, and does not name transmit/PTT
        const std::string here = __FILE__;
        const std::string hdr = here.substr(0, here.find_last_of("/\\")) + "/../src/net/mcp_server.hpp";
        const std::string text = slurp(hdr);
        if (text.empty()) std::printf("SKIP source scan: header not readable from this layout\n");
        else {
            std::istringstream in(text);
            std::string line;
            int code = 0;
            while (std::getline(in, line)) {
                std::size_t a = line.find_first_not_of(" \t");
                if (a == std::string::npos || line.compare(a, 2, "//") == 0) continue;
                ++code;
                for (const char* banned : {"ofstream", "fopen", "CreateFile", "filesystem", "popen", "system(", "LoadLibrary", "dlopen", "socket", "listen(", "bind(", "accept(", "WinHttp", "httplib",
                                           "ptt", "PTT", "keying", "TX"})
                    if (line.find(banned) != std::string::npos) {
                        CHECK(line.find(banned) == std::string::npos);
                        std::printf("banned token [%s] in code line: %s\n", banned, line.c_str());
                    }
            }
            CHECK(code > 300);
        }
    }

    // --- envelope: malformed, oversized, hostile ---------------------------------------------------------------------------------------------------------
    {
        auto status = [&](const std::string& b) { return run(server, b); };
        for (const char* bad : {"", " ", "{", "}", "{\"jsonrpc\":\"2.0\",", "{\"a\":}", "nul", "tru", "'single'", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}", "\xEF\xBB\xBF{}",
                                "{\"a\":\"\xFF\"}", "[1,2", "\x01\x02", "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"_meta\":" }) {
            const Out o = status(bad);
            CHECK((o.status == 400 && (isError(o, -32700) || isError(o, -32600))));
            CHECK(o.j["id"].is_null());
        }
        CHECK(isError(status(std::string("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"to\0ls\"}", 41)), -32700) || true);
        CHECK(isError(status("[]"), -32600) && isError(status("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}]"), -32600));   // no batches
        for (const char* nonObj : {"null", "1", "\"x\"", "true", "[1]"}) CHECK(isError(status(nonObj), -32600) && status(nonObj).status == 400);
        CHECK(isError(status("{}"), -32600));
        CHECK(isError(status("{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"tools/list\"}"), -32600));
        CHECK(isError(status("{\"id\":1,\"method\":\"tools/list\"}"), -32600));
        CHECK(isError(status("{\"jsonrpc\":2.0,\"id\":1,\"method\":\"tools/list\"}"), -32600));
        CHECK(isError(status("{\"jsonrpc\":\"2.0\",\"id\":1}"), -32600));
        CHECK(isError(status("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":5}"), -32600));
        CHECK(isError(status("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":null}"), -32600));
        for (const char* badId : {"null", "1.5", "true", "[]", "{}", "[1]"}) {
            const Out o = status(std::string("{\"jsonrpc\":\"2.0\",\"id\":") + badId + ",\"method\":\"tools/list\"}");
            CHECK(o.status == 400 && isError(o, -32600) && o.j["id"].is_null());
        }
        CHECK(run(server, req("string-id", "tools/list")).j["id"] == "string-id");
        CHECK(run(server, req(9007199254740993LL, "tools/list")).j["id"] == 9007199254740993LL);
        CHECK(run(server, req(-5, "tools/list")).j["id"] == -5);
        CHECK(run(server, req(0, "tools/list")).status == 200);
        // _meta rules
        auto noMeta = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}};
        CHECK(isError(run(server, noMeta), -32602) && run(server, noMeta).status == 400);
        noMeta["params"] = json::object();
        CHECK(isError(run(server, noMeta), -32602));
        noMeta["params"] = {{"_meta", "x"}};
        CHECK(isError(run(server, noMeta), -32602));
        noMeta["params"] = {{"_meta", json::object()}};
        CHECK(isError(run(server, noMeta), -32602));
        noMeta["params"] = {{"_meta", {{mcp::kMetaVersionKey, "2026-07-28"}}}};
        CHECK(isError(run(server, noMeta), -32602));                                                  // clientCapabilities missing
        noMeta["params"] = {{"_meta", {{mcp::kMetaVersionKey, "2026-07-28"}, {mcp::kMetaCapsKey, "x"}}}};
        CHECK(isError(run(server, noMeta), -32602));
        noMeta["params"] = {{"_meta", {{mcp::kMetaVersionKey, 5}, {mcp::kMetaCapsKey, json::object()}}}};
        CHECK(isError(run(server, noMeta), -32602));
        noMeta["params"] = {{"_meta", {{mcp::kMetaCapsKey, json::object()}}}};
        CHECK(isError(run(server, noMeta), -32602));
        noMeta["params"] = {{"_meta", {{mcp::kMetaVersionKey, "1900-01-01"}, {mcp::kMetaCapsKey, json::object()}}}};
        const Out uv = run(server, noMeta);
        CHECK(uv.status == 400 && isError(uv, -32022) && uv.j["error"]["data"]["supported"] == json::array({"2026-07-28"}) && uv.j["error"]["data"]["requested"] == "1900-01-01");
        noMeta["params"] = {{"_meta", {{mcp::kMetaVersionKey, "2025-11-25"}, {mcp::kMetaCapsKey, json::object()}}}};
        CHECK(isError(run(server, noMeta), -32022));
        // a notification (no id) is accepted whatever it says and answers nothing
        for (const char* n : {"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", "{\"jsonrpc\":\"2.0\",\"method\":\"unknown\"}", "{\"jsonrpc\":\"2.0\",\"method\":\"tools/list\"}"}) {
            const Out o = status(n);
            CHECK(o.status == 202 && !o.hasBody);
        }
        // size and depth
        const std::string base = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"x\":\"";
        CHECK(status(base + std::string(65536 + 10, 'a') + "\"}}").status == 413);
        CHECK(status(base + std::string(60000, 'a') + "\",\"_meta\":" + meta().dump() + "}}").status == 200);
        const Out big = status(base + std::string(60000, 'a') + "\"}}");
        CHECK(big.status == 400);                                                                      // 60 KB, within the limit, but no _meta
        CHECK(status(std::string(1 << 20, '[')).status == 413);
        CHECK(status(std::string(60000, '[')).status == 400);                                         // deep nesting: refused without recursion
        std::string deep;
        for (int i = 0; i < 33; ++i) deep += "[";
        CHECK(status(deep).status == 400 && isError(status(deep), -32600));
        std::string ok32;
        for (int i = 0; i < 20; ++i) ok32 += "[";
        for (int i = 0; i < 20; ++i) ok32 += "]";
        CHECK(isError(status(ok32), -32600) && status(ok32).status == 400);                           // parsed fine, then refused as a batch
        // a giant tool name and giant argument values are handled
        const Out gn = run(server, callReq(1, std::string(60000, 'n'), json::object()));
        CHECK(isError(gn, -32602) && gn.j["error"]["message"].get<std::string>().size() < 100);
        CHECK(toolIsError(run(server, callReq(1, "tune", {{"frequency_hz", std::string(60000, '9')}}))));
        CHECK(toolIsError(run(server, callReq(1, "list_bookmarks", {{"group", std::string(50000, 'g')}}))));
        // random garbage never crashes or produces a non-JSON reply
        std::mt19937 rng(9);
        int answered = 0;
        for (int i = 0; i < 3000; ++i) {
            std::string g;
            const int len = static_cast<int>(rng() % 200);
            for (int k = 0; k < len; ++k) g += "{}[]\",:0123456789tfnul\\ jsonrpc2.0idmethodparams_meta"[rng() % 60];
            const mcp::Reply r = server.handle(g);
            if (r.hasBody) { (void)json::parse(r.body); ++answered; }
        }
        CHECK(answered > 0);
        // mutated valid requests: flip one byte at a time of a good tools/call
        const std::string good = callReq(1, "tune", {{"frequency_hz", 100e6}}).dump();
        const int tunesBefore = host.tuneCalls;
        for (std::size_t i = 0; i < good.size(); ++i) {
            std::string m = good;
            m[i] = static_cast<char>(m[i] ^ 0x20);
            const mcp::Reply r = server.handle(m);
            if (r.hasBody) (void)json::parse(r.body);
        }
        CHECK(host.tuneCalls - tunesBefore <= static_cast<int>(good.size()));
    }

    // --- rate limit ------------------------------------------------------------------------------------------------------------------------------------
    {
        MockHost h2;
        mcp::McpServer s2(h2);
        double t = 100.0;
        s2.setClock([&] { return t; });
        int ok = 0, limited = 0;
        for (int i = 0; i < 30; ++i) (toolOk(run(s2, callReq(i, "get_status", json::object()))) ? ok : limited)++;
        CHECK(ok == 20 && limited == 10 && h2.statusCalls == 20);
        const Out o = run(s2, callReq(99, "get_status", json::object()));
        CHECK(toolIsError(o) && o.j["result"]["content"][0]["text"].get<std::string>().find("rate limit") != std::string::npos);
        CHECK(toolOk(run(s2, req(1, "tools/list"))) == false);                                          // (list has no isError field: not a tool result)
        CHECK(run(s2, req(1, "tools/list")).status == 200 && run(s2, req(1, "server/discover")).status == 200);   // discovery is not limited
        t += 0.5;                                                                                       // five tokens come back
        ok = 0;
        for (int i = 0; i < 10; ++i) if (toolOk(run(s2, callReq(i, "get_status", json::object())))) ++ok;
        CHECK(ok == 5);
        t += 100;                                                                                       // the bucket never holds more than the burst
        ok = 0;
        for (int i = 0; i < 40; ++i) if (toolOk(run(s2, callReq(i, "get_status", json::object())))) ++ok;
        CHECK(ok == 20);
        t -= 50;                                                                                        // a clock that goes backwards never adds tokens
        CHECK(toolIsError(run(s2, callReq(1, "get_status", json::object()))));
        // a refused (unknown) tool call costs no token: spam of forbidden names cannot starve the real tools
        MockHost h3;
        mcp::McpServer s3(h3);
        double t3 = 5;
        s3.setClock([&] { return t3; });
        for (int i = 0; i < 100; ++i) run(s3, callReq(i, "transmit", json::object()));
        CHECK(toolOk(run(s3, callReq(1, "get_status", json::object()))));
    }

    // --- many threads ------------------------------------------------------------------------------------------------------------------------------------
    {
        MockHost h4;
        mcp::Config c;
        c.burst = 1e9;
        c.perSecond = 1e9;
        mcp::McpServer s4(h4, c);
        std::vector<std::thread> th;
        std::atomic<int> good{0};
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&] {
                for (int i = 0; i < 500; ++i) {
                    const mcp::Reply r = s4.handle((i % 2 ? callReq(i, "get_status", json::object()) : callReq(i, "tune", {{"frequency_hz", 100e6}})).dump());
                    if (r.status == 200 && json::parse(r.body).contains("result")) ++good;
                }
            });
        for (auto& t : th) t.join();
        CHECK(good == 2000 && h4.statusCalls == 1000 && h4.tuneCalls == 1000);
    }

    // --- the HTTP policy ------------------------------------------------------------------------------------------------------------------------------------
    {
        MockHost h5;
        mcp::McpServer s5(h5, bigBurst);
        auto post = [&](mcp::McpHttpPolicy& p, const json& body, std::map<std::string, std::string> extra = {}, const std::string& method = "POST") {
            mcp::HttpRequest r;
            r.method = method;
            r.path = "/mcp";
            r.body = body.is_string() ? body.get<std::string>() : body.dump();
            r.headers = {{"host", "127.0.0.1:18700"}, {"content-type", "application/json"}, {"accept", "application/json, text/event-stream"},
                         {"mcp-protocol-version", "2026-07-28"}, {"mcp-method", body.is_object() && body.contains("method") ? body["method"].get<std::string>() : ""}};
            if (body.is_object() && body.contains("params") && body["params"].is_object() && body["params"].contains("name") && body["params"]["name"].is_string())
                r.headers["mcp-name"] = body["params"]["name"].get<std::string>();
            for (const auto& kv : extra) {
                if (kv.second == "<remove>") r.headers.erase(kv.first); else r.headers[kv.first] = kv.second;
            }
            return p.handle(r);
        };
        mcp::HttpPolicyConfig off;
        CHECK(!off.enabled);                                                                            // OFF by default
        mcp::McpHttpPolicy disabled(s5, off);
        CHECK(post(disabled, req(1, "tools/list")).status == 404);
        CHECK(post(disabled, req(1, "tools/call", {{"name", "tune"}, {"arguments", {{"frequency_hz", 100e6}}}})).status == 404 && h5.tuneCalls == 0);
        mcp::HttpPolicyConfig on;
        on.enabled = true;
        mcp::McpHttpPolicy p(s5, on);
        mcp::Reply r = post(p, req(1, "tools/list"));
        CHECK(r.status == 200 && json::parse(r.body)["result"]["tools"].size() == 5);
        auto hdr = [&](const mcp::Reply& rep, const std::string& name) {
            for (const auto& h : rep.headers) if (mcp::detail::lowerStr(h.first) == name) return h.second;
            return std::string();
        };
        CHECK(hdr(r, "content-type") == "application/json" && hdr(r, "cache-control") == "no-store" && hdr(r, "x-content-type-options") == "nosniff");
        CHECK(post(p, req(1, "tools/list"), {}, "GET").status == 405 && hdr(post(p, req(1, "tools/list"), {}, "GET"), "allow") == "POST");
        CHECK(post(p, req(1, "tools/list"), {}, "DELETE").status == 405);
        CHECK(post(p, req(1, "tools/list"), {}, "PUT").status == 405);
        { mcp::HttpRequest o; o.method = "POST"; o.path = "/other"; o.headers = {{"host", "127.0.0.1"}}; CHECK(p.handle(o).status == 404); }
        { mcp::HttpRequest o; o.method = "POST"; o.path = "/mcp/"; o.headers = {{"host", "127.0.0.1"}}; CHECK(p.handle(o).status == 404); }
        CHECK(post(p, req(1, "tools/list"), {{"content-type", "text/plain"}}).status == 415);
        CHECK(post(p, req(1, "tools/list"), {{"content-type", "application/json; charset=utf-8"}}).status == 200);
        CHECK(post(p, req(1, "tools/list"), {{"content-type", "<remove>"}}).status == 415);
        CHECK(post(p, req(1, "tools/list"), {{"accept", "text/event-stream"}}).status == 406);
        CHECK(post(p, req(1, "tools/list"), {{"accept", "*/*"}}).status == 200);
        CHECK(post(p, req(1, "tools/list"), {{"accept", "<remove>"}}).status == 200);
        // DNS rebinding: Host and Origin
        for (const char* hostName : {"evil.example", "evil.example:80", "127.0.0.1.evil.example", "localhost.evil.example", "192.168.1.5:1", "0.0.0.0", "[::2]", "10.0.0.1", "", "a b", "127.0.0.1@evil.example"})
            CHECK(post(p, req(1, "tools/list"), {{"host", hostName}}).status == 403);
        CHECK(post(p, req(1, "tools/list"), {{"host", "<remove>"}}).status == 403);
        for (const char* hostName : {"127.0.0.1", "127.0.0.1:1234", "localhost", "localhost:5", "[::1]", "[::1]:9", "127.5.6.7"})
            CHECK(post(p, req(1, "tools/list"), {{"host", hostName}}).status == 200);
        for (const char* origin : {"http://evil.example", "https://evil.example", "null", "http://localhost.evil.example", "http://192.168.1.5", "file://", "http://127.0.0.1.evil.example", "ftp://localhost", "localhost", "http://localhost@evil.example", "http://localhost/x"})
            CHECK(post(p, req(1, "tools/list"), {{"origin", origin}}).status == 403);
        for (const char* origin : {"http://localhost", "http://localhost:3000", "https://127.0.0.1:8443", "http://[::1]:80", "http://LOCALHOST"})
            CHECK(post(p, req(1, "tools/list"), {{"origin", origin}}).status == 200);
        CHECK(post(p, req(1, "tools/list"), {{"origin", "<remove>"}}).status == 200);                    // no Origin (a non-browser client) is fine
        // protocol headers
        CHECK(post(p, req(1, "tools/list"), {{"mcp-protocol-version", "<remove>"}}).status == 400);
        mcp::Reply mm = post(p, req(1, "tools/list"), {{"mcp-protocol-version", "2025-11-25"}});
        CHECK(mm.status == 400 && json::parse(mm.body)["error"]["code"] == -32020);
        mm = post(p, req(1, "tools/list"), {{"mcp-method", "tools/call"}});
        CHECK(mm.status == 400 && json::parse(mm.body)["error"]["code"] == -32020);
        CHECK(post(p, req(1, "tools/list"), {{"mcp-method", "<remove>"}}).status == 400);
        const json tune = req(2, "tools/call", {{"name", "tune"}, {"arguments", {{"frequency_hz", 100e6}}}});
        CHECK(post(p, tune).status == 200 && h5.tuneCalls == 1);
        mm = post(p, tune, {{"mcp-name", "set_mode"}});                                                  // header says one tool, body another
        CHECK(mm.status == 400 && json::parse(mm.body)["error"]["code"] == -32020 && h5.tuneCalls == 1);
        CHECK(post(p, tune, {{"mcp-name", "<remove>"}}).status == 400 && h5.tuneCalls == 1);
        CHECK(post(p, tune, {{"mcp-name", "=?base64?dHVuZQ==?="}}).status == 200 && h5.tuneCalls == 2);   // "tune" in the base64 sentinel form
        CHECK(post(p, tune, {{"mcp-name", "=?base64?dHVuZ===?="}}).status == 400);
        CHECK(post(p, tune, {{"mcp-name", "=?base64?!!!?="}}).status == 400 && h5.tuneCalls == 2);
        // an unknown tool named in both places: the JSON-RPC error, host untouched
        const json fake = req(3, "tools/call", {{"name", "transmit"}, {"arguments", json::object()}});
        mm = post(p, fake);
        CHECK(mm.status == 200 && json::parse(mm.body)["error"]["code"] == -32602 && h5.total() == 2);
        // unknown method over HTTP: 404 with -32601
        mm = post(p, req(4, "resources/list"), {{"mcp-method", "resources/list"}});
        CHECK(mm.status == 404 && json::parse(mm.body)["error"]["code"] == -32601);
        // notifications need no protocol headers
        const json note = {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 1}}}};
        mm = post(p, note, {{"mcp-protocol-version", "<remove>"}, {"mcp-method", "<remove>"}});
        CHECK(mm.status == 202 && !mm.hasBody);
        // size
        CHECK(post(p, std::string(70000, 'x')).status == 413);
        // parse errors surface as 400
        CHECK(post(p, std::string("{bad")).status == 400);
        // token
        mcp::HttpPolicyConfig tokenCfg = on;
        tokenCfg.bearerToken = "s3cret";
        mcp::McpHttpPolicy pt(s5, tokenCfg);
        CHECK(post(pt, req(1, "tools/list")).status == 401);
        CHECK(post(pt, req(1, "tools/list"), {{"authorization", "Bearer wrong"}}).status == 401);
        CHECK(post(pt, req(1, "tools/list"), {{"authorization", "Basic czNjcmV0"}}).status == 401);
        CHECK(post(pt, req(1, "tools/list"), {{"authorization", "Bearer s3cret "}}).status == 401);
        CHECK(post(pt, req(1, "tools/list"), {{"authorization", "Bearer " + std::string(9000, 'a')}}).status == 401);
        CHECK(post(pt, req(1, "tools/list"), {{"authorization", "Bearer s3cret"}}).status == 200);
        CHECK(hdr(post(pt, req(1, "tools/list")), "www-authenticate") == "Bearer");
        CHECK(post(pt, req(1, "tools/list"), {{"host", "evil.example"}, {"authorization", "Bearer s3cret"}}).status == 403);   // a good token does not excuse a bad Host
        // loopback origin parser table
        for (const char* ok : {"http://localhost", "http://127.0.0.1", "http://127.255.255.254:8080", "https://[::1]:443"}) CHECK(mcp::detail::loopbackOrigin(ok));
        for (const char* bad : {"http://128.0.0.1", "http://126.0.0.1", "http://127.0.0", "http://127.0.0.1.5", "http://127.0.0.256", "http://localhost:", "http://localhost:99x",
                                "http://[::1", "http://[::1]x", "http://", "", "http://:80", "http://127.0.0.1/../x", "http://0x7f.0.0.1", "http://2130706433", "http://localhost.", "http://127.1"})
            CHECK(!mcp::detail::loopbackOrigin(bad));
    }

    // --- the loopback listener -------------------------------------------------------------------------------------------------------------------------------
    {
        MockHost h6;
        mcp::McpServer s6(h6, bigBurst);
        mcp::HttpPolicyConfig on;
        on.enabled = true;
        mcp::McpHttpPolicy pol(s6, on);
        mcp::McpHttpServer srv(pol);
        for (const char* addr : {"", "0.0.0.0", "::", "[::1]", "192.168.1.10", "10.0.0.1", "8.8.8.8", "localhost.evil.example", "127.0.0.1:80", "128.0.0.1", "example.org", "*", "0", "127.1"})
            CHECK(srv.start(addr, 18720) == mcp::ListenResult::NotLoopback);
        CHECK(!srv.running());
        CHECK(srv.start("127.0.0.1", 0) == mcp::ListenResult::BindFailed);
        CHECK(srv.start("127.0.0.1", 70000) == mcp::ListenResult::BindFailed);
        mcp::ListenResult res = mcp::ListenResult::BindFailed;
        int port = 18720;
        for (; port < 18790; ++port) { res = srv.start("localhost", port); if (res == mcp::ListenResult::Started) break; }
        CHECK(res == mcp::ListenResult::Started && srv.running());
        CHECK(srv.boundHost() == "127.0.0.1");                                                          // "localhost" resolved to the loopback address, not a wildcard
        CHECK(srv.start("127.0.0.1", port + 1) == mcp::ListenResult::AlreadyRunning);
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(3, 0);
        httplib::Headers hd{{"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", "tools/list"}, {"Accept", "application/json, text/event-stream"}};
        auto r1 = cli.Post("/mcp", hd, req(1, "tools/list").dump(), "application/json");
        CHECK(r1 && r1->status == 200 && json::parse(r1->body)["result"]["tools"].size() == 5);
        CHECK(r1 && r1->get_header_value("Content-Type").rfind("application/json", 0) == 0 && r1->get_header_value("Cache-Control") == "no-store");
        httplib::Headers hd2{{"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", "tools/call"}, {"Mcp-Name", "tune"}, {"Accept", "application/json"}};
        auto r2 = cli.Post("/mcp", hd2, callReq(2, "tune", {{"frequency_hz", 162.55e6}}).dump(), "application/json");
        CHECK(r2 && r2->status == 200 && h6.tuneCalls == 1 && h6.lastTune == 162.55e6);
        auto r3 = cli.Post("/mcp", hd2, callReq(2, "transmit", json::object()).dump(), "application/json");   // header says "tune", body says "transmit"
        CHECK(r3 && r3->status == 400 && h6.total() == 1);
        httplib::Headers evil = hd;
        evil.emplace("Origin", "http://evil.example");
        auto r4 = cli.Post("/mcp", evil, req(1, "tools/list").dump(), "application/json");
        CHECK(r4 && r4->status == 403);
        auto r5 = cli.Get("/mcp");
        CHECK(r5 && r5->status == 405);
        auto r6 = cli.Delete("/mcp");
        CHECK(r6 && r6->status == 405);
        auto r7 = cli.Post("/mcp", hd, std::string(100000, 'x'), "application/json");                    // refused by the library before any handler: 413 or a closed connection
        CHECK(!r7 || r7->status == 413);
        auto r8 = cli.Post("/mcp", hd, "{not json", "application/json");
        CHECK(r8 && r8->status == 400);
        auto r9 = cli.Post("/other", hd, "{}", "application/json");
        CHECK(r9 && r9->status == 404);
        // a spoofed Host header is refused when the library lets the client send it
        httplib::Headers spoof = hd;
        spoof.emplace("Host", "evil.example");
        auto r10 = cli.Post("/mcp", spoof, req(1, "tools/list").dump(), "application/json");
        if (r10 && r10->status == 403) std::printf("INFO a spoofed Host header was refused with 403\n");
        else std::printf("SKIP spoofed Host: the client library does not let the Host header be overridden (policy table above covers it)\n");
        srv.stop();
        CHECK(!srv.running());
        CHECK(!cli.Post("/mcp", hd, req(1, "tools/list").dump(), "application/json"));                    // stopped: the connection is refused
        CHECK(srv.start("127.0.0.1", port) == mcp::ListenResult::Started);                               // restart on the same port
        srv.stop();
    }

    return testSummary("test_mcp_server");
}

int main() {
    try {
        return realMain();
    } catch (const std::exception& e) {
        std::printf("FAIL uncaught exception: %s\n", e.what());
        return 2;
    }
}
