// api_server.cpp - see api_server.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/api_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include <nlohmann/json.hpp>

#include "net/ws_frame.hpp"

namespace cascade::net {
namespace {

using nlohmann::json;

constexpr std::size_t kMaxHeaderLines = 64;
constexpr std::size_t kReadBudgetPerTurn = 64 * 1024;
constexpr int kMaxJsonDepth = 16;

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return s;
}

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) { --b; }
    return s.substr(a, b - a);
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

int jsonDepth(const std::string& s) {
    int depth = 0, maxDepth = 0;
    bool inStr = false, esc = false;
    for (const char c : s) {
        if (inStr) {
            if (esc) { esc = false; }
            else if (c == '\\') { esc = true; }
            else if (c == '"') { inStr = false; }
            continue;
        }
        if (c == '"') { inStr = true; }
        else if (c == '[' || c == '{') { if (++depth > maxDepth) { maxDepth = depth; } }
        else if (c == ']' || c == '}') { if (depth > 0) { --depth; } }
    }
    return maxDepth;
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

}  // namespace

std::string ApiServer::statusJson(const HubStatus& s) {
    json j;
    j["type"] = "status";
    j["running"] = s.running;
    j["tuned_hz"] = s.tunedHz;
    j["centre_hz"] = s.centreHz;
    j["sample_rate_hz"] = s.sampleRateHz;
    j["bandwidth_hz"] = s.bandwidthHz;
    j["signal_db"] = s.signalDb;
    j["mode"] = s.mode;
    j["source"] = s.source;
    return dump(j);
}

std::string ApiServer::bookmarksJson(const std::vector<HubBookmark>& b) {
    json items = json::array();
    for (const HubBookmark& x : b) { items.push_back({{"name", x.name}, {"freq_hz", x.freqHz}, {"mode", x.mode}, {"group", x.group}}); }
    json j;
    j["type"] = "bookmarks";
    j["items"] = std::move(items);
    return dump(j);
}

std::string ApiServer::decodeJson(const HubDecode& d) {
    json j;
    j["type"] = "decode";
    j["seq"] = d.seq;
    j["time"] = d.unixSeconds;
    j["freq_hz"] = d.freqHz;
    j["decoder"] = d.decoder;
    j["text"] = d.text;
    return dump(j);
}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::start(std::string& error) {
    error.clear();
    if (running_.load()) {
        error = "already running";
        return false;
    }
    if (cfg_.maxConnections < 1 || cfg_.maxConnections > 64) {
        error = "the connection limit must be between 1 and 64";
        return false;
    }
    if (cfg_.maxMessageBytes < 256 || cfg_.maxMessageBytes > (1u << 20) || cfg_.clientQueueBytes < 4096 || cfg_.headMs < 100 || cfg_.statusIntervalMs < 50 || cfg_.pollMs < 1 || cfg_.pollMs > 1000) {
        error = "a limit of the interface is out of range";
        return false;
    }
    if (!sock::parseIpv4(cfg_.bindAddress, bindIp_)) {
        error = "'" + cfg_.bindAddress + "' is not an IPv4 address (dotted quad, for example 127.0.0.1)";
        return false;
    }
    if (bindIp_ == 0 && !cfg_.allowAllInterfaces) {
        error = "0.0.0.0 would expose this interface on every network interface; it is refused unless that is asked for explicitly";
        return false;
    }
    if (!sock::ensureStarted()) {
        error = "could not initialise the network stack";
        return false;
    }
    listener_ = sock::listenTcp(bindIp_, cfg_.port, 16, error);
    if (listener_ == sock::kInvalid) { return false; }
    port_.store(sock::boundPort(listener_), std::memory_order_release);
    stopFlag_.store(false);
    running_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(authM_);
        authStop_ = false;
        jobs_.clear();
        done_.clear();
    }
    dropAll_.store(false);
    if (auth_) { authThread_ = std::thread([this] { authLoop(); }); }
    thread_ = std::thread([this] { run(); });
    return true;
}

void ApiServer::stop() {
    if (!thread_.joinable()) { return; }
    stopFlag_.store(true);
    thread_.join();
    {
        std::lock_guard<std::mutex> lk(authM_);
        authStop_ = true;
    }
    authCv_.notify_all();
    if (authThread_.joinable()) { authThread_.join(); }
    running_.store(false, std::memory_order_release);
}

// The auth worker: one sign-in at a time, off the IO thread (a password check is a long derivation, and the IO thread owns every socket).
void ApiServer::authLoop() {
    for (;;) {
        AuthJob job;
        {
            std::unique_lock<std::mutex> lk(authM_);
            authCv_.wait(lk, [this] { return authStop_ || !jobs_.empty(); });
            if (authStop_) { return; }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        const ApiAuth verdict = auth_(job.authorization, job.peer);
        std::lock_guard<std::mutex> lk(authM_);
        done_.push_back({job.id, verdict});
    }
}

bool ApiServer::hostAllowed(const std::string& hostHeader) const {
    if (hostHeader.empty()) { return true; }   // no Host: not a browser (a browser always sends one)
    std::string name = lower(hostHeader);
    if (name.front() == '[') {
        const std::size_t close = name.find(']');
        return close != std::string::npos && name.substr(0, close + 1) == "[::1]" && (close + 1 == name.size() || name[close + 1] == ':');
    }
    const std::size_t colon = name.rfind(':');
    if (colon != std::string::npos) {
        if (colon + 1 == name.size()) { return false; }
        for (std::size_t i = colon + 1; i < name.size(); ++i) { if (name[i] < '0' || name[i] > '9') { return false; } }
        name.resize(colon);
    }
    if (name == "localhost") { return true; }
    std::uint32_t ip = 0;
    if (!sock::parseIpv4(name, ip)) { return false; }   // a name other than localhost is somebody else's (a rebound page names its own site)
    return (ip >> 24) == 127 || ip == bindIp_;
}

ApiStats ApiServer::stats() const {
    ApiStats s;
    s.accepted = accepted_.load();
    s.refusedFull = refusedFull_.load();
    s.refusedRequests = refusedRequests_.load();
    s.wsOpened = wsOpened_.load();
    s.sseOpened = sseOpened_.load();
    s.messages = messages_.load();
    s.errors = errors_.load();
    s.droppedSlow = droppedSlow_.load();
    s.droppedStalled = droppedStalled_.load();
    s.droppedBad = droppedBad_.load();
    s.authDenied = authDenied_.load();
    s.connections = connections_.load();
    return s;
}

void ApiServer::dropConn(std::size_t i, std::atomic<std::uint64_t>* why) {
    const sock::Handle h = conns_[i].s;
    conns_.erase(conns_.begin() + static_cast<std::ptrdiff_t>(i));
    connections_.store(static_cast<int>(conns_.size()), std::memory_order_relaxed);
    if (why != nullptr) { why->fetch_add(1, std::memory_order_relaxed); }
    sock::closeHandle(h);
}

bool ApiServer::flush(Conn& c, std::int64_t now) {
    while (c.pending() > 0) {
        const long r = sock::sendSome(c.s, c.out.data() + c.off, c.pending());
        if (r > 0) {
            c.off += static_cast<std::size_t>(r);
            c.lastProgress = now;
            continue;
        }
        if (r < 0 && sock::wouldBlock()) { break; }
        return false;
    }
    if (c.off == c.out.size()) { c.out.clear(); c.off = 0; }
    else if (c.off > 64 * 1024) { c.out.erase(0, c.off); c.off = 0; }
    return true;
}

bool ApiServer::queue(Conn& c, const std::string& bytes) {
    if (c.pending() + bytes.size() > cfg_.clientQueueBytes) { return false; }
    if (c.pending() == 0) { c.lastProgress = nowMs(); }
    c.out += bytes;
    return true;
}

void ApiServer::respond(Conn& c, int status, const std::string& contentType, const std::string& body, const std::string& extraHeaders) {
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    r += "Content-Type: " + contentType + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
    (void)queue(c, r);
    c.closeAfterSend = true;
    if (status != 200) { refusedRequests_.fetch_add(1, std::memory_order_relaxed); }
}

void ApiServer::closeWs(Conn& c, std::uint16_t code, const char* why) {
    (void)queue(c, ws::encodeClose(code, why));
    c.closeAfterSend = true;
}

void ApiServer::handleHead(Conn& c, std::int64_t now) {
    (void)now;
    const std::size_t end = c.in.find("\r\n\r\n");
    if (end == std::string::npos) { return; }
    const std::string head = c.in.substr(0, end + 4);
    // request line
    const std::size_t eol = head.find("\r\n");
    const std::string line = head.substr(0, eol);
    const std::size_t sp1 = line.find(' ');
    const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { respond(c, 400, "text/plain", "bad request"); return; }
    const std::string method = line.substr(0, sp1), target = line.substr(sp1 + 1, sp2 - sp1 - 1), version = line.substr(sp2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") { respond(c, 505, "text/plain", "http version not supported"); return; }
    // headers
    std::map<std::string, std::string> h;
    std::size_t pos = eol + 2, lines = 0;
    while (pos < head.size()) {
        const std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos || e == pos) { break; }
        if (++lines > kMaxHeaderLines) { respond(c, 431, "text/plain", "too many headers"); return; }
        const std::string hl = head.substr(pos, e - pos);
        pos = e + 2;
        const std::size_t colon = hl.find(':');
        if (colon == std::string::npos || colon == 0) { respond(c, 400, "text/plain", "bad header"); return; }
        h[lower(hl.substr(0, colon))] = trimmed(hl.substr(colon + 1));
    }
    if (method != "GET") { respond(c, 405, "text/plain", "method not allowed", "Allow: GET\r\n"); return; }
    if (h.count("origin") != 0) { respond(c, 403, "text/plain", "a web page may not use this interface"); return; }
    // DNS rebinding: a page that made its own name point at this computer is not this computer's own page. Beyond this computer (requireAuth) every request signs in, which a page cannot do.
    if (cfg_.checkHost && !cfg_.requireAuth) {
        const auto hostIt = h.find("host");
        if (hostIt != h.end() && !hostAllowed(hostIt->second)) { respond(c, 403, "text/plain", "this interface answers to this computer's own address only"); return; }
    }
    if (target != "/status" && target != "/events" && target != "/ws") { respond(c, 404, "text/plain", "not found"); return; }
    // authentication: the caller's (the web page's own account); with requireAuth a request with no credentials never gets as far as the caller
    const auto authIt = h.find("authorization");
    const std::string authorization = authIt == h.end() ? std::string() : authIt->second;
    if (authorization.size() > 1024) { respond(c, 400, "text/plain", "bad request"); return; }
    if (cfg_.requireAuth && authorization.empty()) { finishRequest(c, ApiAuth::Denied, target, head, end, now); return; }
    if (!auth_) { finishRequest(c, ApiAuth::Allowed, target, head, end, now); return; }
    // the sign-in is the worker's: the request waits, the IO thread goes on serving everybody else
    {
        std::lock_guard<std::mutex> lk(authM_);
        if (static_cast<int>(jobs_.size()) >= cfg_.maxPendingAuth) {
            authDenied_.fetch_add(1, std::memory_order_relaxed);
            respond(c, 429, "text/plain", "too many sign-ins at once", "Retry-After: 1\r\n");
            return;
        }
        jobs_.push_back({c.id, authorization, c.peer});
    }
    authCv_.notify_one();
    c.authPending = true;
    c.pendTarget = target;
    c.pendHead = head;
    c.pendEnd = end;
}

// Everything after the sign-in: the verdict, then the answer or the stream.
void ApiServer::finishRequest(Conn& c, ApiAuth verdict, const std::string& target, const std::string& head, std::size_t end, std::int64_t now) {
    c.authPending = false;
    c.pendTarget.clear();
    c.pendHead.clear();
    if (verdict == ApiAuth::TooMany) { authDenied_.fetch_add(1, std::memory_order_relaxed); respond(c, 429, "text/plain", "too many attempts", "Retry-After: 60\r\n"); return; }
    if (verdict != ApiAuth::Allowed) { authDenied_.fetch_add(1, std::memory_order_relaxed); respond(c, 401, "text/plain", "unauthorized", "WWW-Authenticate: Basic realm=\"FoxSDR\"\r\n"); return; }

    if (target == "/status") { respond(c, 200, "application/json", statusJson(hub_.status())); return; }
    if (target == "/events") {
        c.kind = Kind::Sse;
        c.cursor = hub_.newestSeq();   // from now on: no backlog
        (void)queue(c, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: keep-alive\r\n\r\n");
        (void)queue(c, "event: status\ndata: " + statusJson(hub_.status()) + "\n\n");
        sseOpened_.fetch_add(1, std::memory_order_relaxed);
        c.in.clear();
        return;
    }
    // /ws
    const ws::Handshake hs = ws::parseHandshake(head);
    if (!hs.ok) { respond(c, hs.status, "text/plain", hs.error); return; }
    c.kind = Kind::Ws;
    c.decoder = std::make_unique<ws::FrameDecoder>(cfg_.maxMessageBytes);
    c.cursor = hub_.newestSeq();
    (void)queue(c, ws::handshakeAcceptResponse(hs.key));
    (void)queue(c, ws::encodeFrame(ws::Opcode::Text, statusJson(hub_.status())));
    wsOpened_.fetch_add(1, std::memory_order_relaxed);
    // anything the client sent after the head is already a frame
    const std::string rest = c.in.substr(end + 4);
    c.in.clear();
    if (!rest.empty()) { handleWsData(c, rest.data(), rest.size(), now); }
}

void ApiServer::takeAuthResults(std::int64_t now) {
    std::vector<AuthDone> got;
    {
        std::lock_guard<std::mutex> lk(authM_);
        got.swap(done_);
    }
    for (const AuthDone& d : got) {
        for (std::size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].id != d.id) { continue; }
            Conn& c = conns_[i];
            if (c.authPending && !c.closeAfterSend) {
                const std::string target = c.pendTarget, head = c.pendHead;
                finishRequest(c, d.verdict, target, head, c.pendEnd, now);
            }
            break;
        }
    }
}

void ApiServer::answerWs(Conn& c, const std::string& text) {
    messages_.fetch_add(1, std::memory_order_relaxed);
    const auto reply = [&](json j, const json& id) {
        if (!id.is_null()) { j["id"] = id; }
        (void)queue(c, ws::encodeFrame(ws::Opcode::Text, dump(j)));
    };
    if (jsonDepth(text) > kMaxJsonDepth) { errors_.fetch_add(1, std::memory_order_relaxed); reply({{"type", "error"}, {"error", "the message is nested too deeply"}}, json()); return; }
    const json m = json::parse(text, nullptr, false);
    if (m.is_discarded() || !m.is_object()) { errors_.fetch_add(1, std::memory_order_relaxed); reply({{"type", "error"}, {"error", "the message is not a JSON object"}}, json()); return; }
    json id;
    if (m.contains("id") && ((m["id"].is_string() && m["id"].get<std::string>().size() <= 64) || m["id"].is_number_integer())) { id = m["id"]; }
    const std::string op = m.contains("op") && m["op"].is_string() && m["op"].get<std::string>().size() <= 32 ? m["op"].get<std::string>() : std::string();
    const auto fail = [&](const std::string& why) { errors_.fetch_add(1, std::memory_order_relaxed); reply({{"type", "error"}, {"error", why}}, id); };
    if (op == "get_status") {
        reply(json::parse(statusJson(hub_.status())), id);
    } else if (op == "list_bookmarks") {
        reply(json::parse(bookmarksJson(hub_.bookmarks())), id);
    } else if (op == "recent_decodes") {
        std::size_t n = 50;
        if (m.contains("max")) {
            if (!m["max"].is_number_integer() || m["max"].get<long long>() < 1 || m["max"].get<long long>() > static_cast<long long>(kHubMaxDecodes)) { fail("max is a whole number from 1 to 200"); return; }
            n = static_cast<std::size_t>(m["max"].get<long long>());
        }
        json items = json::array();
        for (const HubDecode& d : hub_.recentDecodes(n)) { items.push_back(json::parse(decodeJson(d))); }
        reply({{"type", "decodes"}, {"items", std::move(items)}}, id);
    } else if (op == "tune") {
        if (!m.contains("hz") || !m["hz"].is_number()) { fail("hz is a number"); return; }
        const HubResult r = hub_.requestTune(m["hz"].get<double>());
        if (!r.ok) { fail(r.error); return; }
        reply({{"type", "ok"}, {"op", "tune"}}, id);
    } else if (op == "set_mode") {
        if (!m.contains("mode") || !m["mode"].is_string()) { fail("mode is a text"); return; }
        const HubResult r = hub_.requestMode(m["mode"].get<std::string>());
        if (!r.ok) { fail(r.error); return; }
        reply({{"type", "ok"}, {"op", "set_mode"}}, id);
    } else if (op == "subscribe") {
        if (!m.contains("decodes") || !m["decodes"].is_boolean()) { fail("decodes is true or false"); return; }
        c.wantDecodes = m["decodes"].get<bool>();
        c.cursor = hub_.newestSeq();
        reply({{"type", "ok"}, {"op", "subscribe"}}, id);
    } else {
        fail("unknown operation");
    }
}

void ApiServer::handleWsData(Conn& c, const char* data, std::size_t n, std::int64_t now) {
    (void)now;
    c.decoder->feed(data, n);
    ws::Message msg;
    while (!c.closeAfterSend && c.decoder->next(msg)) {
        switch (msg.op) {
            case ws::Opcode::Text:
                if (!ws::isValidUtf8(msg.payload)) { closeWs(c, ws::kCloseInvalidPayload, "not UTF-8"); droppedBad_.fetch_add(1, std::memory_order_relaxed); break; }
                answerWs(c, msg.payload);
                break;
            case ws::Opcode::Binary:
                closeWs(c, ws::kCloseUnsupportedData, "text only");
                break;
            case ws::Opcode::Ping:
                (void)queue(c, ws::encodeFrame(ws::Opcode::Pong, msg.payload));
                break;
            case ws::Opcode::Close:
                closeWs(c, ws::kCloseNormal, "");
                break;
            default:
                break;
        }
    }
    if (c.decoder->failed() && !c.closeAfterSend) {
        droppedBad_.fetch_add(1, std::memory_order_relaxed);
        closeWs(c, c.decoder->errorCode(), "protocol error");
    }
}

void ApiServer::acceptAll(std::int64_t now) {
    for (;;) {
        const sock::Handle h = sock::acceptOne(listener_);
        if (h == sock::kInvalid) { return; }
        if (static_cast<int>(conns_.size()) >= cfg_.maxConnections) {
            refusedFull_.fetch_add(1, std::memory_order_relaxed);
            sock::closeHandle(h);
            continue;
        }
        const std::string peer = sock::peerOf(h);
        if (cfg_.maxPerAddressHead > 0) {
            int same = 0;
            for (const Conn& o : conns_) { if (o.kind == Kind::Head && o.peer == peer) { ++same; } }
            if (same >= cfg_.maxPerAddressHead) {
                refusedFull_.fetch_add(1, std::memory_order_relaxed);   // one address holding the slots with requests it never finishes
                sock::closeHandle(h);
                continue;
            }
        }
        sock::setNoDelay(h);
        Conn c;
        c.s = h;
        c.id = nextId_++;
        c.peer = peer;
        c.born = now;
        c.lastProgress = now;
        conns_.push_back(std::move(c));
        accepted_.fetch_add(1, std::memory_order_relaxed);
        connections_.store(static_cast<int>(conns_.size()), std::memory_order_relaxed);
    }
}

void ApiServer::run() {
    std::vector<sock::PollItem> items;
    std::int64_t lastStatus = nowMs();
    while (!stopFlag_.load(std::memory_order_acquire)) {
        std::int64_t now = nowMs();
        takeAuthResults(now);
        if (dropAll_.exchange(false, std::memory_order_acq_rel)) {   // the account changed: no stream that signed in under the old one stays (a request still waiting for its sign-in is judged under the new one)
            for (std::size_t i = conns_.size(); i-- > 0;) { if (conns_[i].kind != Kind::Head) { dropConn(i, nullptr); } }
        }
        // the periodic push: the status to every stream, and the new decodes to those that want them
        if (now - lastStatus >= cfg_.statusIntervalMs) {
            lastStatus = now;
            const std::string status = statusJson(hub_.status());
            for (std::size_t i = 0; i < conns_.size();) {
                Conn& c = conns_[i];
                bool ok = true;
                if (c.kind == Kind::Ws && !c.closeAfterSend) { ok = queue(c, ws::encodeFrame(ws::Opcode::Text, status)); }
                else if (c.kind == Kind::Sse) { ok = queue(c, "event: status\ndata: " + status + "\n\n"); }
                if (!ok) { dropConn(i, &droppedSlow_); continue; }
                ++i;
            }
        }
        for (std::size_t i = 0; i < conns_.size();) {
            Conn& c = conns_[i];
            bool ok = true;
            if ((c.kind == Kind::Sse || (c.kind == Kind::Ws && c.wantDecodes)) && !c.closeAfterSend) {
                std::uint64_t cur = c.cursor;
                for (const HubDecode& d : hub_.decodesSince(cur, 20)) {
                    const std::string j = decodeJson(d);
                    ok = c.kind == Kind::Ws ? queue(c, ws::encodeFrame(ws::Opcode::Text, j)) : queue(c, "event: decode\ndata: " + j + "\n\n");
                    if (!ok) { break; }
                }
                c.cursor = cur;
            }
            if (!ok) { dropConn(i, &droppedSlow_); continue; }
            ++i;
        }
        // push what is queued; judge stalls and slow heads
        for (std::size_t i = 0; i < conns_.size();) {
            Conn& c = conns_[i];
            if (!flush(c, now)) { dropConn(i, nullptr); continue; }
            if (c.closeAfterSend && c.pending() == 0) { dropConn(i, nullptr); continue; }
            if (cfg_.stallMs > 0 && c.pending() > 0 && now - c.lastProgress >= cfg_.stallMs) { dropConn(i, &droppedStalled_); continue; }
            if (c.kind == Kind::Head && now - c.born >= cfg_.headMs) { dropConn(i, &droppedBad_); continue; }
            ++i;
        }
        items.clear();
        sock::PollItem l;
        l.s = listener_;
        l.want = sock::kReadable;
        items.push_back(l);
        for (const Conn& c : conns_) {
            sock::PollItem it;
            it.s = c.s;
            it.want = sock::kReadable | (c.pending() > 0 ? sock::kWritable : 0u);
            items.push_back(it);
        }
        if (sock::pollMany(items.data(), items.size(), cfg_.pollMs) < 0) { continue; }
        now = nowMs();
        if (items[0].got & sock::kReadable) { acceptAll(now); }
        const std::size_t polled = items.size() - 1;
        for (std::size_t k = polled; k-- > 0;) {
            if (k >= conns_.size()) { continue; }
            const unsigned got = items[k + 1].got;
            if (got == 0) { continue; }
            Conn& c = conns_[k];
            bool gone = false;
            if ((got & sock::kReadable) && !c.closeAfterSend) {
                std::size_t budget = kReadBudgetPerTurn;
                char buf[2048];
                while (budget > 0 && !gone) {
                    const long r = sock::recvSome(c.s, buf, std::min<std::size_t>(sizeof(buf), budget));
                    if (r > 0) {
                        budget -= static_cast<std::size_t>(r);
                        if (c.kind == Kind::Head && c.authPending) {
                            c.in.append(buf, static_cast<std::size_t>(r));   // what a client sends while its sign-in is checked is kept (a WebSocket may send its first frame at once), bounded
                            if (c.in.size() > ws::kMaxHandshakeBytes + 2 * cfg_.maxMessageBytes + 64) { droppedBad_.fetch_add(1, std::memory_order_relaxed); gone = true; }
                        } else if (c.kind == Kind::Head) {
                            c.in.append(buf, static_cast<std::size_t>(r));
                            if (c.in.size() > ws::kMaxHandshakeBytes && c.in.find("\r\n\r\n") == std::string::npos) { respond(c, 431, "text/plain", "request too large"); break; }
                            handleHead(c, now);
                            if (c.closeAfterSend) { break; }
                        } else if (c.kind == Kind::Ws) {
                            handleWsData(c, buf, static_cast<std::size_t>(r), now);
                            if (c.closeAfterSend) { break; }
                        }   // an SSE client's bytes are thrown away
                        continue;
                    }
                    if (r < 0 && sock::wouldBlock()) { break; }
                    gone = true;
                }
            }
            if (!gone && (got & sock::kError)) { gone = true; }
            if (!gone && (got & sock::kWritable) && !flush(c, now)) { gone = true; }
            if (gone) { dropConn(k, nullptr); }
        }
    }
    for (Conn& c : conns_) { sock::closeHandle(c.s); }
    conns_.clear();
    connections_.store(0, std::memory_order_relaxed);
    sock::closeHandle(listener_);
    listener_ = sock::kInvalid;
}

}  // namespace cascade::net
