// discord_presence.cpp - see discord_presence.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/discord_presence.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::net {

namespace discord {

namespace {
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) { b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

std::vector<std::uint8_t> encodeFrame(Op op, const std::string& payload) {
    std::vector<std::uint8_t> b;
    b.reserve(8 + payload.size());
    put32(b, static_cast<std::uint32_t>(op));
    put32(b, static_cast<std::uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

bool FrameParser::feed(const std::uint8_t* data, std::size_t n, std::vector<Frame>& out) {
    if (!error_.empty()) { return false; }
    buf_.insert(buf_.end(), data, data + n);
    std::size_t at = 0;
    while (buf_.size() - at >= 8) {
        const std::uint32_t op = get32(buf_.data() + at);
        const std::uint32_t len = get32(buf_.data() + at + 4);
        if (op > 4) { error_ = "unknown opcode"; return false; }
        if (len > kMaxFrameBytes) { error_ = "frame too long"; return false; }
        if (buf_.size() - at < 8 + static_cast<std::size_t>(len)) { break; }
        out.push_back(Frame{static_cast<Op>(op), std::string(reinterpret_cast<const char*>(buf_.data() + at + 8), len)});
        at += 8 + len;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
}

bool validClientId(const std::string& id) {
    if (id.size() < 17 || id.size() > 20) { return false; }
    return std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string capUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) { return s; }
    std::size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) { --n; }   // s[n] would start in the middle of a character: back up to its start
    return s.substr(0, n);
}

std::string handshakePayload(const std::string& clientId) { return "{\"v\":1,\"client_id\":\"" + jsonEscape(clientId) + "\"}"; }

std::string activityJson(const std::string& details, const std::string& state) {
    std::string j = "{\"type\":2";
    if (!details.empty()) { j += ",\"details\":\"" + jsonEscape(capUtf8(details)) + "\""; }
    if (!state.empty()) { j += ",\"state\":\"" + jsonEscape(capUtf8(state)) + "\""; }
    return j + "}";
}

std::string setActivityPayload(std::uint32_t pid, const std::string& nonce, const std::string& activity) {
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid);
    if (!activity.empty()) { j += ",\"activity\":" + activity; }
    j += "},\"nonce\":\"" + jsonEscape(nonce) + "\"}";
    return j;
}

namespace {
// The text after `"key"` and its colon, whitespace skipped; npos when the key is absent.
std::size_t valueAfter(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    std::size_t at = j.find(k);
    while (at != std::string::npos) {
        std::size_t p = at + k.size();
        while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) { ++p; }
        if (p < j.size() && j[p] == ':') {
            ++p;
            while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\r' || j[p] == '\n')) { ++p; }
            return p;
        }
        at = j.find(k, at + 1);
    }
    return std::string::npos;
}
}  // namespace

ReplyInfo parseReply(const std::string& payload) {
    ReplyInfo r;
    const std::size_t evt = valueAfter(payload, "evt");
    if (evt != std::string::npos) {
        if (payload.compare(evt, 7, "\"READY\"") == 0) { r.kind = Reply::Ready; }
        else if (payload.compare(evt, 7, "\"ERROR\"") == 0) { r.kind = Reply::Error; }
    }
    if (r.kind == Reply::Error) {
        const std::size_t code = valueAfter(payload, "code");
        if (code != std::string::npos) {
            long v = 0;
            std::size_t p = code;
            bool any = false;
            while (p < payload.size() && payload[p] >= '0' && payload[p] <= '9' && v < 1000000) { v = v * 10 + (payload[p] - '0'); ++p; any = true; }
            r.code = any ? static_cast<int>(v) : 0;
        }
    }
    return r;
}

}  // namespace discord

std::string formatFrequency(double hz) {
    if (!std::isfinite(hz) || hz <= 0.0) { return std::string(); }
    char buf[48];
    if (hz >= 1.0e6) {
        std::snprintf(buf, sizeof buf, "%.3f MHz", hz / 1.0e6);
    } else if (hz >= 1.0e3) {
        std::snprintf(buf, sizeof buf, "%.3f", hz / 1.0e3);
        std::string s = buf;
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
        return s + " kHz";
    } else {
        std::snprintf(buf, sizeof buf, "%.0f Hz", hz);
    }
    return buf;
}

PresenceText composePresence(const PresenceInput& in, bool showFrequencyMode) {
    PresenceText t;
    if (!in.listening) { return t; }
    t.show = true;
    t.details = "Listening to the radio";
    if (showFrequencyMode) {
        std::string mode;
        for (char c : in.mode) {   // a demodulation mode's name: letters, digits and a few marks, nothing else
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '/' || c == ' ') { mode += c; }
            if (mode.size() >= 16) { break; }
        }
        const std::string f = formatFrequency(in.frequencyHz);
        if (!f.empty()) { t.state = mode.empty() ? f : f + " " + mode; }
        else { t.state = mode; }
    }
    return t;
}

#if defined(_WIN32)

using Clock = std::chrono::steady_clock;

struct DiscordPresence::Impl {
    DiscordOptions opt;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    bool stopping = false;
    // what is wanted, and what was last sent (under m)
    PresenceText want;
    PresenceText sent;
    bool sentAny = false;
    bool dirty = false;
    std::string stateWord = "off";
    std::size_t frames = 0, connectCount = 0, errors = 0;
    std::atomic<bool> clearRequested{false};

    // worker-only
    HANDLE pipe = INVALID_HANDLE_VALUE;
    discord::FrameParser parser;
    unsigned nonce = 0;

    void setState(const char* w) {
        std::lock_guard<std::mutex> lock(m);
        stateWord = w;
    }
    bool write(const std::vector<std::uint8_t>& bytes) {
        std::size_t at = 0;
        while (at < bytes.size()) {
            DWORD wrote = 0;
            if (!::WriteFile(pipe, bytes.data() + at, static_cast<DWORD>(bytes.size() - at), &wrote, nullptr) || wrote == 0) { return false; }
            at += wrote;
        }
        return true;
    }
    void closePipe() {
        if (pipe != INVALID_HANDLE_VALUE) { ::CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
        parser = discord::FrameParser();
    }
    bool openPipe() {
        for (int n = 0; n < opt.maxPipes; ++n) {
            const std::string name = opt.pipeBase + std::to_string(n);
            std::wstring w(name.begin(), name.end());
            HANDLE h = ::CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
            if (h != INVALID_HANDLE_VALUE) { pipe = h; return true; }
        }
        return false;
    }
    // Reads what has arrived (never blocks). false: the connection is gone or broke the protocol.
    bool poll(std::vector<discord::FrameParser::Frame>& frames_) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) { return false; }
        while (avail > 0) {
            std::uint8_t buf[4096];
            DWORD got = 0;
            if (!::ReadFile(pipe, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) { return false; }
            if (!parser.feed(buf, got, frames_)) { return false; }
            avail -= std::min(avail, got);
        }
        return true;
    }
    // Handles frames that are not the handshake's answer: ping to pong, close to lost, errors counted. false: drop the connection.
    bool handle(const std::vector<discord::FrameParser::Frame>& fs) {
        for (const auto& f : fs) {
            if (f.op == discord::Op::Ping) {
                if (!write(discord::encodeFrame(discord::Op::Pong, f.payload))) { return false; }
            } else if (f.op == discord::Op::Close) {
                return false;
            } else if (f.op == discord::Op::Frame) {
                if (discord::parseReply(f.payload).kind == discord::Reply::Error) {
                    std::lock_guard<std::mutex> lock(m);
                    ++errors;
                }
            }
        }
        return true;
    }
    // The handshake and the wait for READY. 1 ready, 0 refused, -1 lost.
    int handshake() {
        if (!write(discord::encodeFrame(discord::Op::Handshake, discord::handshakePayload(opt.clientId)))) { return -1; }
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (Clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(m);
                if (stopping) { return -1; }
            }
            std::vector<discord::FrameParser::Frame> fs;
            if (!poll(fs)) { return -1; }
            int verdict = -2;   // -2: not yet; every frame of the batch is handled before answering, so a PING that follows READY is not lost
            for (const auto& f : fs) {
                if (f.op == discord::Op::Ping) { write(discord::encodeFrame(discord::Op::Pong, f.payload)); continue; }
                if (f.op == discord::Op::Close) { if (verdict == -2) { verdict = 0; } continue; }
                if (f.op == discord::Op::Frame && verdict == -2) {
                    const auto r = discord::parseReply(f.payload);
                    if (r.kind == discord::Reply::Ready) { verdict = 1; }
                    else if (r.kind == discord::Reply::Error) { verdict = 0; }
                }
            }
            if (verdict != -2) { return verdict; }
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(opt.tickMs / 2, 1)));
        }
        return -1;
    }
    bool sendActivity(const PresenceText& t) {
        const std::string payload = discord::setActivityPayload(opt.pid != 0 ? opt.pid : ::GetCurrentProcessId(), std::to_string(++nonce),
                                                                 t.show ? discord::activityJson(t.details, t.state) : std::string());
        return write(discord::encodeFrame(discord::Op::Frame, payload));
    }

    void run() {
        int retry = opt.retryMs;
        auto nextTry = Clock::now();
        auto lastSend = Clock::now() - std::chrono::hours(1);
        bool connected = false;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait_for(lock, std::chrono::milliseconds(opt.tickMs), [&] { return stopping; });
                if (stopping) { break; }
            }
            if (!connected) {
                if (Clock::now() < nextTry) { continue; }
                setState("searching");
                if (!openPipe()) {
                    nextTry = Clock::now() + std::chrono::milliseconds(retry);
                    retry = std::min(retry * 2, opt.maxRetryMs);
                    continue;
                }
                setState("connecting");
                const int h = handshake();
                if (h == 1) {
                    connected = true;
                    retry = opt.retryMs;
                    std::lock_guard<std::mutex> lock(m);
                    stateWord = "connected";
                    ++connectCount;
                    dirty = true;          // say what the line is now
                    sentAny = false;
                } else {
                    closePipe();
                    setState(h == 0 ? "refused" : "lost");
                    nextTry = Clock::now() + std::chrono::milliseconds(retry);
                    retry = std::min(retry * 2, opt.maxRetryMs);
                }
                continue;
            }
            std::vector<discord::FrameParser::Frame> fs;
            if (!poll(fs) || !handle(fs)) {
                closePipe();
                connected = false;
                setState("lost");
                nextTry = Clock::now() + std::chrono::milliseconds(std::min(retry, 1000 + opt.retryMs / 4));
                continue;
            }
            PresenceText toSend;
            bool send = false;
            {
                std::lock_guard<std::mutex> lock(m);
                const bool same = sentAny && sent.show == want.show && sent.details == want.details && sent.state == want.state;
                if (dirty && same) { dirty = false; }
                if (dirty && Clock::now() - lastSend >= std::chrono::milliseconds(opt.minIntervalMs)) {
                    if (!want.show && !sentAny) { dirty = false; }   // nothing was ever shown and nothing is wanted: nothing to clear
                    else { toSend = want; send = true; }
                }
            }
            if (send) {
                if (!sendActivity(toSend)) {
                    closePipe();
                    connected = false;
                    setState("lost");
                    nextTry = Clock::now() + std::chrono::milliseconds(retry);
                    continue;
                }
                lastSend = Clock::now();
                std::lock_guard<std::mutex> lock(m);
                sent = toSend;
                sentAny = true;
                dirty = !(want.show == sent.show && want.details == sent.details && want.state == sent.state);
                ++frames;
            }
        }
        // the end: take the line down, best effort
        if (connected) {
            bool shown;
            { std::lock_guard<std::mutex> lock(m); shown = sentAny && sent.show; }
            if (shown) { PresenceText none; sendActivity(none); std::lock_guard<std::mutex> lock(m); ++frames; }
        }
        closePipe();
    }
};

DiscordPresence::DiscordPresence(DiscordOptions options) : impl_(std::make_unique<Impl>()) {
    Impl& I = *impl_;
    I.opt = std::move(options);
    if (I.opt.clientId.empty()) { I.stateWord = "off"; return; }        // OFF: no thread, no pipe
    if (!discord::validClientId(I.opt.clientId)) { I.stateWord = "bad-id"; return; }
    I.stateWord = "searching";
    I.worker = std::thread([&I] { I.run(); });
}

DiscordPresence::~DiscordPresence() {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lock(I.m);
        I.stopping = true;
    }
    I.cv.notify_all();
    if (I.worker.joinable()) { I.worker.join(); }
}

void DiscordPresence::update(const PresenceInput& in) {
    Impl& I = *impl_;
    if (!I.worker.joinable()) { return; }
    const PresenceText t = composePresence(in, I.opt.showFrequencyMode);
    std::lock_guard<std::mutex> lock(I.m);
    if (t.show == I.want.show && t.details == I.want.details && t.state == I.want.state) { return; }
    I.want = t;
    I.dirty = true;
}

void DiscordPresence::clear() {
    Impl& I = *impl_;
    if (!I.worker.joinable()) { return; }
    std::lock_guard<std::mutex> lock(I.m);
    I.want = PresenceText();
    I.dirty = true;
}

std::string DiscordPresence::state() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->stateWord; }
std::size_t DiscordPresence::framesSent() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->frames; }
std::size_t DiscordPresence::connects() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->connectCount; }
std::size_t DiscordPresence::errorsSeen() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->errors; }

#else  // not Windows: Discord's socket lives elsewhere and is not built here; the object is inert

struct DiscordPresence::Impl {
    DiscordOptions opt;
    std::string stateWord = "off";
};
DiscordPresence::DiscordPresence(DiscordOptions o) : impl_(std::make_unique<Impl>()) { impl_->opt = std::move(o); }
DiscordPresence::~DiscordPresence() = default;
void DiscordPresence::update(const PresenceInput&) {}
void DiscordPresence::clear() {}
std::string DiscordPresence::state() const { return impl_->stateWord; }
std::size_t DiscordPresence::framesSent() const { return 0; }
std::size_t DiscordPresence::connects() const { return 0; }
std::size_t DiscordPresence::errorsSeen() const { return 0; }

#endif

}  // namespace cascade::net
