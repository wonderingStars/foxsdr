// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// Headless provisioning (engine extraction stage 7e). See provision.hpp for what it is and why it is shaped so.
#include "engine/provision.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/server_host.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {

namespace {

// A password is a line of text a person typed: more than this is not one (a binary file named by mistake).
// The engine's own limit (256 bytes, parseServerConfigRequest) is the one that decides what a password may be;
// this only keeps a wrong file from being read whole.
constexpr std::size_t kMaxPasswordFileBytes = 4096;

// Strict UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF): what the JSON text must be.
bool validUtf8(const std::string& s) {
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (c < 0x80u) {
            ++i;
            continue;
        } else if (c >= 0xC2u && c <= 0xDFu) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + len > n) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) { return false; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800u) || (len == 4 && cp < 0x10000u)) { return false; }  // overlong
        if (cp >= 0xD800u && cp <= 0xDFFFu) { return false; }                             // a surrogate
        if (cp > 0x10FFFFu) { return false; }
        i += len;
    }
    return true;
}

// A JSON string literal for `s`: quotes, backslashes and control characters escaped, the rest (UTF-8 included)
// as it is. The settings panel's own (AppWindow's serverJsonQuote), which lives in a window file this binary
// does not link; tests/test_engine_provision.cpp feeds what this writes to the engine's own parser.
void jsonQuoteInto(std::string& out, const std::string& s) {
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

std::string jsonQuote(const std::string& s) {
    std::string out;
    jsonQuoteInto(out, s);
    return out;
}

const char* kFlag = "--web-password-file";

// The text of a setting fact, or empty.
std::string factText(const FoxEngineApi* api, FoxSession* s, const char* key) {
    char buf[2048];
    const int32_t n = api->get_setting(s, key, buf, sizeof(buf));
    if (n < 0) { return std::string(); }
    const std::size_t len = std::min(static_cast<std::size_t>(n), sizeof(buf) - 1u);
    return std::string(buf, len);
}

// One listener as the SERVERS list says it: whether it listens, on which port, and the sentence it was refused with.
struct Listener {
    bool read = false;
    bool running = false;
    int port = 0;
    std::string refusal;
};

Listener readListener(const FoxEngineApi* api, FoxSession* s, std::uint32_t index) {
    Listener l;
    FoxListItem it{};
    it.structSize = sizeof(it);
    // `<= index` is "not there": a list that changed under the read.
    if (api->read_list(s, FOXAPI_LIST_SERVERS, index, &it) <= static_cast<int32_t>(index)) { return l; }
    l.read = true;
    l.running = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
    l.port = static_cast<int>(it.value[0]);
    l.refusal = it.detail;
    return l;
}

// Closes the session when the provisioning is over, on every path out of it.
struct SessionGuard {
    const FoxEngineApi* api = nullptr;
    FoxSession* s = nullptr;
    ~SessionGuard() {
        if (api != nullptr && s != nullptr) { api->close_session(s); }
    }
};

}  // namespace

void Secret::wipe(std::string& s) {
    // Volatile stores: the buffer is about to be freed, and a plain fill is a dead store the optimiser may remove.
    volatile char* p = s.empty() ? nullptr : &s[0];
    for (std::size_t i = 0; i < s.size(); ++i) { p[i] = '\0'; }
    s.clear();
}

bool readPasswordFrom(std::FILE* in, Secret& out, std::string& why) {
    out.value.clear();
    // Reserved up front so the plaintext is never copied to a new block while it grows (a freed block would keep it).
    std::string raw;
    raw.reserve(kMaxPasswordFileBytes + 1u);
    char chunk[256];
    while (raw.size() <= kMaxPasswordFileBytes) {
        const std::size_t want = std::min(sizeof(chunk), kMaxPasswordFileBytes + 1u - raw.size());
        const std::size_t got = std::fread(chunk, 1, want, in);
        raw.append(chunk, got);
        if (got < want) { break; }
    }
    volatile char* wipeChunk = chunk;
    for (std::size_t i = 0; i < sizeof(chunk); ++i) { wipeChunk[i] = '\0'; }

    const auto fail = [&](const char* reason) {
        Secret::wipe(raw);
        why = reason;
        return false;
    };
    if (std::ferror(in) != 0) { return fail("could not be read"); }
    if (raw.size() > kMaxPasswordFileBytes) { return fail("holds more than a password can be"); }
    if (!raw.empty() && raw.back() == '\n') { raw.pop_back(); }
    if (!raw.empty() && raw.back() == '\r') { raw.pop_back(); }
    if (raw.empty()) { return fail("holds no password"); }
    for (const char ch : raw) {
        if (ch == '\n' || ch == '\r' || ch == '\0') { return fail("must hold the password on one line, and nothing else"); }
    }
    if (!validUtf8(raw)) { return fail("is not valid UTF-8 text"); }
    out.value = std::move(raw);  // (a move: the one buffer, not a copy)
    return true;
}

bool readPassword(const std::string& source, Secret& out, std::string& error) {
    const bool stdinSource = source == "-";
    const std::string name = stdinSource ? std::string("standard input") : "\"" + source + "\"";
    std::FILE* f = stdinSource ? stdin : std::fopen(source.c_str(), "rb");
    if (f == nullptr) {
        error = std::string(kFlag) + ": cannot open " + name;
        return false;
    }
    std::string why;
    const bool ok = readPasswordFrom(f, out, why);
    if (!stdinSource) { std::fclose(f); }
    if (!ok) { error = std::string(kFlag) + ": " + name + " " + why; }
    return ok;
}

std::string serverConfigText(const ProvisionRequest& r, const Secret* password) {
    // The password's member is quoted into its own string and the pieces are joined into blocks reserved at their final
    // size, so the plaintext is copied as few times as it can be and each copy is overwritten before it is released.
    std::string member;
    std::string web;
    const std::size_t room = 512u + (password != nullptr ? password->value.size() * 6u : 0u);
    web.reserve(room);
    const auto add = [&web](const std::string& m) {
        if (!web.empty()) { web += ','; }
        web += m;
    };
    if (r.webEnable) {
        add("\"enabled\":true");
        if (!r.webBind.empty()) { add("\"bindAddress\":" + jsonQuote(r.webBind)); }
        if (r.webPort != 0) { add("\"port\":" + std::to_string(r.webPort)); }
        if (!r.webUser.empty()) { add("\"username\":" + jsonQuote(r.webUser)); }
        if (password != nullptr && !password->empty()) {
            member.reserve(room);
            member += "\"password\":";
            jsonQuoteInto(member, password->value);
            add(member);
            Secret::wipe(member);
        }
    }
    std::string cat;
    if (r.catEnable) {
        cat = "\"enabled\":true";
        if (r.catBindAll) { cat += ",\"bindAll\":true"; }
    }
    std::string text;
    text.reserve(room + 128u);
    text += '{';
    if (!web.empty()) {
        text += "\"web\":{";
        text += web;
        text += '}';
    }
    if (!cat.empty()) {
        if (!web.empty()) { text += ','; }
        text += "\"cat\":{";
        text += cat;
        text += '}';
    }
    text += '}';
    Secret::wipe(web);
    return text;
}

ProvisionOutcome provisionServers(Engine& engine, const ProvisionRequest& request, const Secret* password,
                                  std::chrono::milliseconds bound) {
    ProvisionOutcome out;
    if (!request.any()) {
        out.ok = true;
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() + bound;
    const auto expired = [&deadline] { return std::chrono::steady_clock::now() >= deadline; };
    const auto fail = [&out](std::string why) {
        out.ok = false;
        out.error = std::move(why);
        return out;
    };

    const FoxEngineApi* api = engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    if (api == nullptr) { return fail("foxsdr-engine: the engine's interface is not available"); }

    // A LOCAL session with the ADMIN grant: the one kind SERVER_CONFIG accepts (it changes what listens on this
    // machine; no REMOTE session is ever given ADMIN). VIEW so the lists and facts below can be read.
    FoxSessionParams p{};
    p.structSize = sizeof(p);
    p.flags = FOXAPI_SESSION_LOCAL;
    p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_ADMIN;
    p.clientName = "foxsdr-engine provisioning";
    SessionGuard session;
    session.api = api;
    if (api->open_session(engine.apiHandle(), &p, &session.s) != FOXAPI_OK || session.s == nullptr) {
        session.s = nullptr;
        return fail("foxsdr-engine: the engine would not open a session to be provisioned on");
    }

    // SUBMIT. The text is longer than a command's own slot, so it goes by submit_text. It holds the password
    // when there is one: overwritten as soon as the engine has taken it (the engine's copy is hashed on its
    // worker and overwritten there).
    std::string text = serverConfigText(request, password);
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = FOXAPI_OP_SERVER_CONFIG;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t taken = api->submit_text(session.s, &c, text.data(), static_cast<std::uint32_t>(text.size()), &sr);
    Secret::wipe(text);
    if (taken < 0 || sr.status != FOXAPI_OK) {
        return fail("foxsdr-engine: the engine did not take the provisioning request (status " +
                    std::to_string(static_cast<int>(taken < 0 ? taken : sr.status)) + ")");
    }

    // THE ANSWER: the passes are run by hand (there is no control thread yet), a millisecond apart.
    FoxCommandResult answer{};
    bool answered = false;
    while (!answered && !expired()) {
        engine.pump();
        FoxCommandResult r[8];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api->poll_results(session.s, r, 8);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                answer = r[i];
                answered = true;
            }
        }
        if (!answered) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    if (!answered) { return fail("foxsdr-engine: the engine did not answer the provisioning request in time"); }
    // NO_CHANGE is "the configuration already says that" (a restored file that holds the same): not a refusal.
    if (answer.status != FOXAPI_OK && answer.status != FOXAPI_NO_CHANGE) {
        std::string why = answer.message;
        if (why.empty()) { why = "status " + std::to_string(static_cast<int>(answer.status)); }
        return fail("foxsdr-engine: the web/CAT settings were refused: " + why);
    }

    // SETTLED: the password hashed (its record is what the bind policy reads; the listener waits for it), the
    // host's worker idle (it has started or refused what it was asked to), then two passes so the list and the
    // facts carry the host's report.
    ServerHost& host = engine.servers();
    while (host.passwordPending() && !expired()) {
        engine.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (host.passwordPending()) { return fail("foxsdr-engine: the web password was not hashed in time"); }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (!host.waitIdle(left.count() > 0 ? left : std::chrono::milliseconds(1))) {
        return fail("foxsdr-engine: the web and CAT listeners did not settle in time");
    }
    engine.pump();
    engine.pump();

    // WHAT THE ENGINE SAYS OF EACH LISTENER THE FLAGS ASKED FOR. A listener that is not up is a refusal, and the
    // sentence is the engine's: the bind policy's for a configuration it will not let listen (an off-machine bind
    // with no password), the machine's for one it could not bind (a port that is taken). The list's detail is cut
    // at 160 characters; the policy's fact is the whole sentence, and is used when it is the same sentence longer
    // (the settings panel's rule, AppWindow::serverRefusalText).
    std::string summary;
    if (request.webEnable) {
        const Listener w = readListener(api, session.s, 0);
        if (!w.read || !w.running) {
            std::string why = w.refusal;
            const std::string policy = factText(api, session.s, "fact.web.policy");
            if (policy.size() > why.size() && policy.compare(0, why.size(), why) == 0) { why = policy; }
            if (why.empty()) { why = "the web server did not start"; }
            return fail("foxsdr-engine: the web server was not started: " + why);
        }
        const std::string bind = factText(api, session.s, "fact.web.bindAddress");
        const bool auth = factText(api, session.s, "fact.web.authRequired") == "1";
        summary += "web on " + bind + ":" + std::to_string(w.port) + (auth ? " (sign-in required)" : " (no password: loopback only)");
    }
    if (request.catEnable) {
        const Listener k = readListener(api, session.s, 1);
        if (!k.read || !k.running) {
            std::string why = k.refusal.empty() ? std::string("the CAT listener did not start") : k.refusal;
            return fail("foxsdr-engine: the CAT listener was not started: " + why);
        }
        summary += std::string(summary.empty() ? "" : ", ") + "CAT on " +
                   (request.catBindAll ? "all interfaces" : "127.0.0.1") + ":" + std::to_string(k.port) +
                   (request.catBindAll ? " (CAT has no password: anything on the network can retune this receiver)" : "");
    }
    out.ok = true;
    out.summary = summary;
    return out;
}

}  // namespace cascade::engine
