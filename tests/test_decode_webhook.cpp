// Tests for the decode JSON (core/decode_json.hpp) and the webhook (core/decode_webhook.hpp). The JSON is read back by an INDEPENDENT parser
// (nlohmann::json, which the project already uses for its settings; none of it is used by the formatter) over thousands of random strings; the
// webhook posts only to a stand-in HTTP server on 127.0.0.1 inside the test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decode_json.hpp"
#include "core/decode_webhook.hpp"

#include "test_check.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <thread>

#include "http_post_stub.hpp"

namespace dj = cascade::core::decodejson;
namespace wh = cascade::core::webhook;
using nlohmann::json;

namespace {

// Appends code point cp as UTF-8.
void putCp(std::string& s, unsigned cp) {
    if (cp < 0x80) { s += static_cast<char>(cp); }
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// A random VALID string over an alphabet that hurts: quote, backslash, controls, DEL, slash, separators, multi-byte, astral.
std::string randomValid(std::mt19937& g, std::size_t maxLen) {
    static const unsigned pool[] = {'"', '\\', '/', '\n', '\r', '\t', '\b', '\f', 0x01, 0x1F, 0x7F, ' ', 'a', 'Z', '0', '{', '}', '[', ']', ':', ',', 0xA0, 0xE9,
                                    0x20AC, 0x2028, 0x2029, 0xFFFD, 0x1F600, 0x10FFFF, 0x10000, 0x7FF, 0x800, 0xFFFF};
    std::string s;
    const std::size_t n = g() % (maxLen + 1);
    for (std::size_t i = 0; i < n; ++i) { putCp(s, pool[g() % (sizeof pool / sizeof pool[0])]); }
    return s;
}

dj::DecodeEvent sample() {
    dj::DecodeEvent e;
    e.plugin = "ft8";
    e.unixMs = 1794414024123LL;
    e.frequencyHz = 14074000.0;
    e.text = "CQ DX K1ABC FN42";
    e.fields.push_back({"snr", "", -12.0, true});
    e.fields.push_back({"call", "K1ABC", 0, false});
    return e;
}

void testShape() {
    const std::string s = dj::toJson(sample());
    const json j = json::parse(s);
    CHECK(j["plugin"] == "ft8" && j["time"] == "2026-11-11T16:20:24.123Z" && j["unix_ms"] == 1794414024123LL);
    CHECK(j["frequency_hz"] == 14074000.0 && j["text"] == "CQ DX K1ABC FN42");
    CHECK(j["fields"]["snr"] == -12.0 && j["fields"]["call"] == "K1ABC");
    CHECK(!j.contains("truncated"));
    // golden text: the exact bytes
    CHECK(s == "{\"plugin\":\"ft8\",\"time\":\"2026-11-11T16:20:24.123Z\",\"unix_ms\":1794414024123,\"frequency_hz\":14074000,\"text\":\"CQ DX K1ABC FN42\","
               "\"fields\":{\"snr\":-12,\"call\":\"K1ABC\"}}");
    CHECK(dj::isoUtc(0) == "1970-01-01T00:00:00.000Z" && dj::isoUtc(-1) == "1969-12-31T23:59:59.999Z");
    // no frequency: omitted; non-finite field: null; no fields: an empty object; duplicate keys: the first kept
    dj::DecodeEvent e = sample();
    e.frequencyHz = std::nan("");
    e.fields.clear();
    e.fields.push_back({"x", "", INFINITY, true});
    e.fields.push_back({"k", "one", 0, false});
    e.fields.push_back({"k", "two", 0, false});
    json k = json::parse(dj::toJson(e));
    CHECK(!k.contains("frequency_hz") && k["fields"]["x"].is_null() && k["fields"]["k"] == "one" && k["fields"].size() == 2);
    e.fields.clear();
    k = json::parse(dj::toJson(e));
    CHECK(k["fields"].is_object() && k["fields"].empty());
    // frequency with a fraction round-trips exactly
    e.frequencyHz = 7074000.123456789;
    CHECK(json::parse(dj::toJson(e))["frequency_hz"].get<double>() == 7074000.123456789);
    // truncation on a character boundary
    e.text = std::string(10, 'a') + "\xE2\x82\xAC" + std::string(10, 'b');  // the euro sign spans bytes 10..12
    const json t = json::parse(dj::toJson(e, 11));                           // would cut inside the euro sign
    CHECK(t["truncated"] == true && t["text"] == std::string(10, 'a'));
    const json t2 = json::parse(dj::toJson(e, 13));
    CHECK(t2["text"] == std::string(10, 'a') + "\xE2\x82\xAC" && t2["truncated"] == true);
    CHECK(!json::parse(dj::toJson(e, 100)).contains("truncated"));
}

void testEscapingAgainstIndependentParser() {
    std::mt19937 g(2026);
    int n = 0;
    for (int i = 0; i < 6000; ++i) {
        dj::DecodeEvent e;
        e.plugin = randomValid(g, 8);
        e.text = randomValid(g, 60);
        e.unixMs = static_cast<std::int64_t>(g()) * 1000 + static_cast<std::int64_t>(g() % 1000);
        e.frequencyHz = static_cast<double>(g() % 3000000000u) / 4.0;
        const std::size_t nf = g() % 4;
        std::map<std::string, std::string> want;
        for (std::size_t f = 0; f < nf; ++f) {
            dj::Field fl;
            fl.key = randomValid(g, 6) + "k" + std::to_string(f);  // distinct keys
            fl.text = randomValid(g, 20);
            e.fields.push_back(fl);
            want[fl.key] = fl.text;
        }
        const std::string s = dj::toJson(e, 100000);
        const json j = json::parse(s, nullptr, false);
        CHECK(!j.is_discarded());
        if (j.is_discarded()) { continue; }
        CHECK(j["plugin"].get<std::string>() == e.plugin);
        CHECK(j["text"].get<std::string>() == e.text);
        CHECK(j["fields"].size() == want.size());
        for (const auto& [k, v] : want) { CHECK(j["fields"][k].get<std::string>() == v); }
        // no raw control character survives in the output text
        for (unsigned char c : s) { CHECK(c >= 0x20 && c != 0x7F); }
        ++n;
    }
    CHECK(n == 6000);
    // the escapes themselves (golden)
    CHECK(dj::escapeString("a\"b\\c/\n\r\t\b\f\x01\x1f\x7f") == "a\\\"b\\\\c/\\n\\r\\t\\b\\f\\u0001\\u001f\\u007f");
    CHECK(dj::escapeString("") == "" && dj::escapeString("plain") == "plain");
    // a value ending in a backslash cannot swallow the closing quote
    dj::DecodeEvent e = sample();
    e.text = "ends with \\";
    CHECK(json::parse(dj::toJson(e))["text"] == "ends with \\");
    // text that looks like JSON stays text
    e.text = "\",\"fields\":{\"injected\":1},\"x\":\"";
    const json inj = json::parse(dj::toJson(e));
    CHECK(inj["text"] == e.text && !inj["fields"].contains("injected") && inj["fields"].size() == 2);
}

void testInvalidUtf8() {
    // every byte string, valid or not, becomes valid JSON in valid UTF-8; invalid bytes become U+FFFD
    std::mt19937 g(9);
    for (int i = 0; i < 4000; ++i) {
        dj::DecodeEvent e = sample();
        e.text.clear();
        const std::size_t len = g() % 24;
        for (std::size_t k = 0; k < len; ++k) {
            const unsigned r = g() % 8;
            e.text += static_cast<char>(r < 4 ? g() % 256 : (r < 6 ? 0x80 + g() % 0x40 : 0xC0 + g() % 0x40));
        }
        const std::string s = dj::toJson(e);
        const json j = json::parse(s, nullptr, false);  // nlohmann rejects invalid UTF-8 in the document
        CHECK(!j.is_discarded());
    }
    struct Row { const char* in; const char* out; };
    const Row rows[] = {{"\xFF", "\xEF\xBF\xBD"}, {"a\x80z", "a\xEF\xBF\xBD" "z"}, {"\xC0\xAF", "\xEF\xBF\xBD\xEF\xBF\xBD"}, {"\xE2\x82", "\xEF\xBF\xBD\xEF\xBF\xBD"},
                        {"\xED\xA0\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"}, {"\xF4\x90\x80\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},
                        {"\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80"}, {"\xE2\x82\xAC", "\xE2\x82\xAC"}};
    for (const Row& r : rows) { CHECK(dj::escapeString(r.in) == r.out); }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
void testWebhook() {
    const std::string body = dj::toJson(sample());
    // off by default: nothing sent
    {
        StubServer s("/hook");
        CHECK(s.start(StubServer::Mode::Accept200));
        wh::Options o;
        o.url = s.url();
        const wh::Result r = wh::send(o, body);
        CHECK(!r.attempted && !r.ok && r.error == "disabled");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(s.connections() == 0);
    }
    // a 200: the body arrives byte for byte, as JSON
    {
        StubServer s("/hook");
        CHECK(s.start(StubServer::Mode::Accept200));
        wh::Options o;
        o.enabled = true;
        o.url = s.url();
        const wh::Result r = wh::send(o, body);
        CHECK(r.attempted && r.ok && r.status == 200 && r.error.empty());
        const auto bodies = s.bodies();
        CHECK(bodies.size() == 1 && !bodies.empty() && bodies[0] == body);
        const auto reqs = s.requests();
        CHECK(!reqs.empty() && reqs[0].find("POST /hook") == 0);
        bool ctype = false;
        for (const std::string& rq : reqs) { std::string low; for (char c : rq) { low += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } ctype = ctype || low.find("content-type: application/json") != std::string::npos; }
        CHECK(ctype);
    }
    // 400 and 429: not ok, the status in the text, the URL not
    for (auto mode : {StubServer::Mode::BadRequest400, StubServer::Mode::RateLimit429}) {
        StubServer s("/secret-token-path");
        CHECK(s.start(mode));
        wh::Options o;
        o.enabled = true;
        o.url = s.url() + "?key=hunter2";
        const wh::Result r = wh::send(o, body);
        CHECK(r.attempted && !r.ok && (r.status == 400 || r.status == 429));
        CHECK(r.error == "HTTP " + std::to_string(r.status));
        CHECK(r.error.find("hunter2") == std::string::npos && r.error.find("secret-token") == std::string::npos);
    }
    // refused connection: no answer, no crash
    {
        StubServer s("/hook");
        CHECK(s.start(StubServer::Mode::Refuse));
        wh::Options o;
        o.enabled = true;
        o.url = s.url();
        const wh::Result r = wh::send(o, body);
        CHECK(!r.ok && r.status == 0 && !r.error.empty());
    }
    // cancel a request that is never answered
    {
        StubServer s("/hook");
        CHECK(s.start(StubServer::Mode::Hang));
        wh::Options o;
        o.enabled = true;
        o.url = s.url();
        auto c = std::make_shared<cascade::core::UploadCancel>();
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); c->cancel(); });
        const auto t0 = std::chrono::steady_clock::now();
        const wh::Result r = wh::send(o, body, c);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t.join();
        std::printf("webhook to a server that never answers, cancelled after 300 ms: returned after %.0f ms (%s)\n", ms, r.error.c_str());
        CHECK(!r.ok && ms < 5000);
    }
    // local refusals: never reach the network
    wh::Options o;
    o.enabled = true;
    o.url = "ftp://example.org/x";
    CHECK(!wh::send(o, body).attempted);
    o.url = "http://127.0.0.1:1/x y";
    CHECK(wh::send(o, body).error == "bad URL");
    o.url = "http://127.0.0.1:1/x\r\nHost: evil";
    CHECK(wh::send(o, body).error == "bad URL");
    o.url = std::string("http://127.0.0.1:1/") + std::string(3000, 'a');
    CHECK(wh::send(o, body).error == "bad URL");
    o.url = "http://127.0.0.1:1/x";
    CHECK(wh::send(o, "").error == "nothing to send");
    // plain http to a non-loopback address is refused by the existing client BEFORE any connection (192.0.2.1 is the documentation range)
    o.url = "http://192.0.2.1/hook";
    const wh::Result lan = wh::send(o, body);
    CHECK(!lan.attempted && !lan.ok && lan.error.find("plain http") != std::string::npos);
}

}  // namespace

int main() {
    testShape();
    testEscapingAgainstIndependentParser();
    testInvalidUtf8();
    testWebhook();
    return testSummary("test_decode_webhook");
}
