// Tests for screen lane S4's engine half (engine/s4_outputs.hpp): the decoded-text feed sent to MQTT and a webhook, and the keyword alerts.
// EVERYTHING TALKS TO STAND-INS INSIDE THIS TEST on 127.0.0.1 (a scripted broker, a stub web server) and the notification sink is a recording stand-in: nothing leaves the machine,
// nothing is shown or heard on the desktop.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4_outputs.hpp"

#include "test_check.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "http_post_stub.hpp"

namespace s4 = cascade::engine::s4;
namespace al = cascade::core::alerts;
namespace mq = cascade::core::mqtt;
namespace pld = cascade::core::powerlog::detail;
using nlohmann::json;

namespace {

s4::Event ev(std::uint64_t seq, const std::string& text, bool priv = false, std::int64_t ms = 1794414024123LL, const std::string& plugin = "pocsag") {
    s4::Event e;
    e.seq = seq;
    e.unixMs = ms;
    e.frequencyHz = 153350000.0;
    e.plugin = plugin;
    e.kind = "text";
    e.text = text;
    e.priv = priv;
    return e;
}

// A scripted stand-in MQTT broker: accepts one connection at a time, answers CONNECT with a CONNACK, and keeps every PUBLISH it is sent.
class Broker {
public:
    ~Broker() { stop(); }
    bool start() {
        pld::ensureNet();
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (pld::badSock(listen_)) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { return false; }
#if defined(_WIN32)
        int len = sizeof a;
#else
        socklen_t len = sizeof a;
#endif
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        if (::listen(listen_, 4) != 0) { return false; }
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_ = false;
        if (th_.joinable()) { th_.join(); }
        if (!pld::badSock(listen_)) { pld::closeSock(listen_); listen_ = static_cast<pld::sock_t>(-1); }
    }
    int port() const { return port_; }
    std::vector<std::pair<std::string, std::string>> published() { std::lock_guard<std::mutex> lk(mu_); return pub_; }
    std::string user() { std::lock_guard<std::mutex> lk(mu_); return user_; }

private:
    static bool readable(pld::sock_t c, int ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(c, &r);
        timeval tv{0, ms * 1000};
#if defined(_WIN32)
        return ::select(0, &r, nullptr, nullptr, &tv) > 0;
#else
        return ::select(c + 1, &r, nullptr, nullptr, &tv) > 0;
#endif
    }
    void loop() {
        while (run_) {
            if (!readable(listen_, 50)) { continue; }
            pld::sock_t c = ::accept(listen_, nullptr, nullptr);
            if (pld::badSock(c)) { continue; }
            serve(c);
            pld::closeSock(c);
        }
    }
    void serve(pld::sock_t c) {
        std::string in;
        while (run_) {
            if (!readable(c, 50)) { continue; }
            char buf[4096];
            const int n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) { return; }
            in.append(buf, static_cast<std::size_t>(n));
            for (;;) {
                if (in.size() < 2) { break; }
                std::size_t rl = 0, used = 0;
                const auto st = mq::decodeRemainingLength(reinterpret_cast<const unsigned char*>(in.data()) + 1, in.size() - 1, &rl, &used);
                if (st == mq::RlStatus::NeedMore) { break; }
                if (st == mq::RlStatus::Malformed) { return; }
                const std::size_t total = 1 + used + rl;
                if (in.size() < total) { break; }
                const int type = static_cast<unsigned char>(in[0]) >> 4;
                const unsigned char* b = reinterpret_cast<const unsigned char*>(in.data()) + 1 + used;
                if (type == 1) {
                    std::size_t o = 0;
                    auto str = [&]() { const std::size_t l = (b[o] << 8) | b[o + 1]; std::string s(reinterpret_cast<const char*>(b) + o + 2, l); o += 2 + l; return s; };
                    str();
                    o += 1;
                    const int flags = b[o++];
                    o += 2;
                    str();
                    if ((flags & 0x80) != 0) { std::lock_guard<std::mutex> lk(mu_); user_ = str(); }
                    const char ack[4] = {0x20, 0x02, 0x00, 0x00};
                    ::send(c, ack, 4, 0);
                } else if (type == 3) {
                    const std::size_t tl = (b[0] << 8) | b[1];
                    std::string topic(reinterpret_cast<const char*>(b) + 2, tl);
                    std::string payload(reinterpret_cast<const char*>(b) + 2 + tl, rl - 2 - tl);
                    std::lock_guard<std::mutex> lk(mu_);
                    pub_.emplace_back(std::move(topic), std::move(payload));
                } else if (type == 14) {
                    return;
                }
                in.erase(0, total);
            }
        }
    }
    pld::sock_t listen_ = static_cast<pld::sock_t>(-1);
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::thread th_;
    std::mutex mu_;
    std::vector<std::pair<std::string, std::string>> pub_;
    std::string user_;
};

template <class F>
bool waitFor(F f, int ms = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

void testPureRules() {
    // keywords: trimmed, empty lines and (case-insensitive) repeats dropped
    s4::KeywordList k = s4::parseKeywords("  mayday \n\nMAYDAY\nPAN PAN\r\n");
    CHECK(k.problem.empty() && k.keywords.size() == 2 && k.keywords[0] == "mayday" && k.keywords[1] == "PAN PAN");
    CHECK(s4::parseKeywords("").keywords.empty() && s4::parseKeywords("").problem.empty());
    // boundaries: 64 is fine, 65 is refused; 120 bytes fine, 121 refused; a control byte refused
    std::string sixtyFour, sixtyFive;
    for (int i = 0; i < 65; ++i) { sixtyFive += "k" + std::to_string(i) + "\n"; if (i < 64) { sixtyFour += "k" + std::to_string(i) + "\n"; } }
    CHECK(s4::parseKeywords(sixtyFour).keywords.size() == 64 && s4::parseKeywords(sixtyFour).problem.empty());
    CHECK(!s4::parseKeywords(sixtyFive).problem.empty() && s4::parseKeywords(sixtyFive).keywords.empty());
    CHECK(s4::parseKeywords(std::string(120, 'a')).problem.empty());
    CHECK(!s4::parseKeywords(std::string(121, 'a')).problem.empty());
    CHECK(!s4::parseKeywords(std::string("ab\x01" "cd")).problem.empty());

    // webhook address
    CHECK(s4::checkWebhookUrl("").empty());
    CHECK(s4::checkWebhookUrl("https://example.org/hook?t=1").empty() && s4::checkWebhookUrl("http://127.0.0.1:8080/x").empty());
    CHECK(!s4::checkWebhookUrl("ftp://example.org").empty() && !s4::checkWebhookUrl("https://a b").empty() && !s4::checkWebhookUrl(std::string("https://a\nb")).empty());
    CHECK(s4::checkWebhookUrl("https://" + std::string(2040, 'a')).empty());
    CHECK(!s4::checkWebhookUrl("https://" + std::string(2041, 'a')).empty());
    // MQTT host and topic
    CHECK(s4::checkMqttHost("broker.local").empty() && s4::checkMqttHost("127.0.0.1").empty());
    CHECK(!s4::checkMqttHost("").empty() && !s4::checkMqttHost("a b").empty() && !s4::checkMqttHost("a/b").empty() && !s4::checkMqttHost("u@h").empty());
    CHECK(s4::checkMqttTopic("foxsdr/decodes").empty() && !s4::checkMqttTopic("").empty() && !s4::checkMqttTopic("a/#").empty() && !s4::checkMqttTopic("a/+/b").empty());
    CHECK(!s4::checkMqttTopic(std::string(201, 't')).empty() && s4::checkMqttTopic(std::string(200, 't')).empty());
    // who can read what is sent
    CHECK(s4::isLoopbackHost("127.0.0.1") && s4::isLoopbackHost("localhost") && s4::isLoopbackHost("::1") && !s4::isLoopbackHost("192.168.1.5") && !s4::isLoopbackHost("example.org"));
    CHECK(s4::mqttScope("127.0.0.1", true) == "this computer only");
    CHECK(s4::mqttScope("192.168.1.5", true).find("without encryption") != std::string::npos);
    CHECK(s4::hookScope("http://127.0.0.1:1/x") == "this computer only");
    CHECK(s4::hookScope("http://192.168.1.9/x").find("refused") != std::string::npos);
    CHECK(s4::hookScope("https://example.org/x").find("encrypted") != std::string::npos);
    CHECK(s4::hookScope("").empty());
}

void testOffDoesNothing() {
    s4::Hub h;
    CHECK(!h.active());
    h.ingest({ev(1, "mayday mayday")});
    CHECK(h.stats().seen == 0);
    s4::Config c;   // the defaults: everything off
    h.configure(c);
    CHECK(!h.active());
    h.ingest({ev(2, "mayday")});
    CHECK(h.stats().seen == 0 && h.stats().alertsRaised == 0 && h.stats().mqttState == "off");
    h.stop();
}

void testAlerts() {
    s4::Hub h;
    auto rec = std::make_shared<al::RecordingSink>();
    h.setNotifier(rec);
    s4::Config c;
    c.alerts = true;
    c.keywords = "mayday\nPAN PAN";
    c.alertGapS = 30;
    h.configure(c);
    CHECK(h.active());
    h.ingest({ev(1, "this is a MAYDAY call", false, 1000000), ev(2, "weather report", false, 1001000), ev(3, "mayday again", false, 1002000),   // the same keyword within 30 s: once
              ev(4, "pan pan pan", false, 1003000), ev(5, "mayday from a private plugin", true, 1004000), ev(6, "mayday", false, 1000000 + 31000)});
    CHECK(h.drain(5000));
    const s4::Stats st = h.stats();
    CHECK(st.seen == 6 && st.heldPrivate == 1);
    CHECK(st.alertsRaised == 3);     // mayday (1), pan pan (4), mayday again after the gap (6); the repeat inside the gap and the private line raise nothing
    CHECK(st.notified == 3 && rec->items().size() == 3 && st.notifyFailed == 0);
    CHECK(h.recentAlerts().size() == 3);
    bool leaked = false;
    for (const al::Notification& n : rec->items()) { if (n.body.find("private plugin") != std::string::npos) { leaked = true; } }
    CHECK(!leaked);

    // the notification switch off: alerts are raised and listed, the sink is not called
    s4::Hub h2;
    auto rec2 = std::make_shared<al::RecordingSink>();
    h2.setNotifier(rec2);
    c.alertNotify = false;
    h2.configure(c);
    h2.ingest({ev(1, "mayday", false, 5000)});
    CHECK(h2.drain(2000) && h2.stats().alertsRaised == 1 && rec2->items().empty());

    // a keyword list with a fault: the problem is said and nothing is raised (never a half-loaded list)
    s4::Hub h3;
    c.alertNotify = true;
    c.keywords = std::string(300, 'x');
    h3.configure(c);
    CHECK(!h3.stats().alertProblem.empty());
    h3.ingest({ev(1, "xxx", false, 1)});
    CHECK(h3.stats().alertsRaised == 0);

    // a sink that refuses is counted and its sentence kept; a sink that throws does not end the worker
    struct Refusing : al::NotificationSink {
        const char* name() const override { return "refusing"; }
        bool notify(const al::Notification&, std::string* err) override { if (err) { *err = "the desktop refused"; } return false; }
    };
    struct Throwing : al::NotificationSink {
        const char* name() const override { return "throwing"; }
        bool notify(const al::Notification&, std::string*) override { throw 1; }
    };
    s4::Hub h4;
    h4.setNotifier(std::make_shared<Refusing>());
    c.keywords = "mayday";
    h4.configure(c);
    h4.ingest({ev(1, "mayday", false, 1)});
    CHECK(h4.drain(2000) && h4.stats().notifyFailed == 1 && h4.stats().notifyProblem == "the desktop refused");
    h4.setNotifier(std::make_shared<Throwing>());
    h4.ingest({ev(2, "mayday", false, 99000000)});
    CHECK(h4.drain(2000) && h4.stats().notifyFailed == 2);
    h4.ingest({ev(3, "mayday", false, 199000000)});
    CHECK(h4.drain(2000) && h4.stats().notifyFailed == 3);   // the worker is still alive
}

void testWebhook() {
    // stand-in client: every decoded line is one POST of the decode JSON; the address never reaches the stats
    {
        s4::Hub h;
        std::mutex mu;
        std::vector<std::string> seen;
        std::string seenUrl;
        h.setPoster([&](const std::string& url, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            seen.push_back(body);
            seenUrl = url;
            cascade::core::webhook::Result r;
            r.attempted = true;
            r.ok = true;
            r.status = 200;
            return r;
        });
        s4::Config c;
        c.hook = true;
        c.hookUrl = "https://example.org/secret-token-123";
        h.configure(c);
        h.ingest({ev(1, "CQ test", false), ev(2, "private line", true), ev(3, "second", false)});
        CHECK(h.drain(5000));
        std::lock_guard<std::mutex> lk(mu);
        CHECK(seen.size() == 2 && seenUrl == c.hookUrl);
        const json j = json::parse(seen[0]);
        CHECK(j["plugin"] == "pocsag" && j["text"] == "CQ test" && j["frequency_hz"] == 153350000.0);
        const s4::Stats st = h.stats();
        CHECK(st.hookOk == 2 && st.hookFailed == 0 && st.heldPrivate == 1);
        CHECK(st.hookProblem.find("secret-token") == std::string::npos);
    }
    // a failing client: counted, one sentence, the address not in it
    {
        s4::Hub h;
        h.setPoster([](const std::string&, const std::string&) {
            cascade::core::webhook::Result r;
            r.attempted = true;
            r.status = 500;
            r.error = "HTTP 500";
            return r;
        });
        s4::Config c;
        c.hook = true;
        c.hookUrl = "https://example.org/secret-token-123";
        h.configure(c);
        h.ingest({ev(1, "x")});
        CHECK(h.drain(5000));
        CHECK(h.stats().hookFailed == 1 && h.stats().hookProblem == "HTTP 500");
    }
    // a bad address, or none: said at once, nothing queued
    {
        s4::Hub h;
        s4::Config c;
        c.hook = true;
        c.hookUrl = "gopher://x";
        h.configure(c);
        h.ingest({ev(1, "x")});
        CHECK(!h.stats().hookProblem.empty() && h.stats().hookQueued == 0);
        s4::Hub h2;
        c.hookUrl.clear();
        h2.configure(c);
        CHECK(h2.stats().hookProblem == "an address is needed");
    }
    // alerts only: a decoded line that matches nothing is not posted; an alert is
    {
        s4::Hub h;
        std::mutex mu;
        std::vector<std::string> seen;
        h.setPoster([&](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            seen.push_back(body);
            cascade::core::webhook::Result r;
            r.attempted = true;
            r.ok = true;
            r.status = 200;
            return r;
        });
        s4::Config c;
        c.hook = true;
        c.hookUrl = "https://example.org/h";
        c.hookAlerts = true;
        c.alerts = true;
        c.keywords = "fire";
        h.configure(c);
        h.ingest({ev(1, "all quiet", false, 1000), ev(2, "FIRE at the depot", false, 2000)});
        CHECK(h.drain(5000));
        std::lock_guard<std::mutex> lk(mu);
        CHECK(seen.size() == 1);
        if (seen.size() == 1) {
            const json j = json::parse(seen[0]);
            CHECK(j["fields"]["alert"] == "keywords" && j["fields"]["matched"] == "keyword: fire" && j["text"] == "FIRE at the depot");
        }
    }
    // the real bounded client to a stub on this computer: the body arrives; and plain http to another address is refused by the client itself (no connection is tried)
    {
        StubServer stub("/hook");
        CHECK(stub.start(StubServer::Mode::Accept200));
        s4::Hub h;
        s4::Config c;
        c.hook = true;
        c.hookUrl = stub.url();
        h.configure(c);
        h.ingest({ev(1, "over the wire")});
        CHECK(h.drain(10000));
        CHECK(waitFor([&] { return !stub.bodies().empty(); }, 3000));
        const auto bodies = stub.bodies();
        CHECK(bodies.size() == 1 && bodies[0].find("over the wire") != std::string::npos);
        CHECK(h.stats().hookOk == 1);
        s4::Hub h2;
        c.hookUrl = "http://192.0.2.1/x";   // a documentation address: never reached, the client refuses plain http off this computer
        h2.configure(c);
        h2.ingest({ev(1, "no")});
        CHECK(h2.drain(10000));
        CHECK(h2.stats().hookFailed == 1 && h2.stats().hookOk == 0 && h2.stats().hookProblem.find("plain http") != std::string::npos);
    }
    // the queue is bounded: a client that never answers does not grow memory without bound
    {
        s4::Hub h;
        std::atomic<bool> release{false};
        h.setPoster([&](const std::string&, const std::string&) {
            while (!release) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            cascade::core::webhook::Result r;
            r.attempted = true;
            r.ok = true;
            r.status = 200;
            return r;
        });
        s4::Config c;
        c.hook = true;
        c.hookUrl = "https://example.org/h";
        h.configure(c);
        std::vector<s4::Event> many;
        for (int i = 1; i <= 1000; ++i) { many.push_back(ev(static_cast<std::uint64_t>(i), "line")); }
        h.ingest(many);
        const s4::Stats st = h.stats();
        CHECK(st.hookDropped >= 800);   // at most about 100 wait; the rest are dropped and counted
        release = true;
        CHECK(h.drain(10000));
    }
}

void testMqtt() {
    Broker b;
    CHECK(b.start());
    s4::Hub h;
    s4::Config c;
    c.mqtt = true;
    c.mqttHost = "127.0.0.1";
    c.mqttPort = b.port();
    c.mqttTopic = "foxsdr/test";
    c.mqttUser = "someone";
    c.mqttPassword = "hunter2";
    h.configure(c);
    CHECK(waitFor([&] { return h.stats().mqttState == "connected"; }));
    h.ingest({ev(1, "first line"), ev(2, "private", true), ev(3, "third line")});
    CHECK(waitFor([&] { return b.published().size() >= 2; }));
    const auto got = b.published();
    CHECK(got.size() == 2 && got[0].first == "foxsdr/test" && got[1].first == "foxsdr/test");
    if (got.size() == 2) {
        CHECK(json::parse(got[0].second)["text"] == "first line" && json::parse(got[1].second)["text"] == "third line");
    }
    CHECK(b.user() == "someone");
    // the stats never hold the password or the user name
    const s4::Stats st = h.stats();
    CHECK(st.mqttProblem.find("hunter2") == std::string::npos && st.mqttOffered == 2);
    // switched off: the connection is closed and nothing more is sent
    c.mqtt = false;
    h.configure(c);
    CHECK(h.stats().mqttState == "off");
    h.ingest({ev(4, "after off")});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(b.published().size() == 2);
    h.stop();

    // alerts only: the alert goes out as JSON, the plain line does not
    Broker b2;
    CHECK(b2.start());
    s4::Hub h2;
    s4::Config c2;
    c2.mqtt = true;
    c2.mqttPort = b2.port();
    c2.mqttAlerts = true;
    c2.alerts = true;
    c2.keywords = "fire";
    h2.configure(c2);
    CHECK(waitFor([&] { return h2.stats().mqttState == "connected"; }));
    h2.ingest({ev(1, "quiet", false, 1000), ev(2, "fire!", false, 2000)});
    CHECK(waitFor([&] { return b2.published().size() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto got2 = b2.published();
    CHECK(got2.size() == 1);
    if (got2.size() == 1) { CHECK(json::parse(got2[0].second)["fields"]["alert"] == "keywords"); }
    h2.stop();

    // a bad host or topic is said and nothing starts
    s4::Hub h3;
    s4::Config c3;
    c3.mqtt = true;
    c3.mqttHost = "bad host";
    h3.configure(c3);
    CHECK(!h3.stats().mqttProblem.empty());
    s4::Hub h4;
    c3.mqttHost = "127.0.0.1";
    c3.mqttTopic = "a/#";
    h4.configure(c3);
    CHECK(!h4.stats().mqttProblem.empty());
}

void testConfigChange() {
    // a change of a setting re-applies; the same configuration again is a no-op; stop() returns the hub to off
    s4::Hub h;
    s4::Config c;
    c.alerts = true;
    c.keywords = "a";
    h.configure(c);
    CHECK(h.active());
    h.configure(c);
    c.keywords = "b";
    h.configure(c);
    h.ingest({ev(1, "has b in it", false, 1)});
    CHECK(h.stats().alertsRaised == 1);
    h.stop();
    CHECK(!h.active());
}

}  // namespace

int main() {
    testPureRules();
    testOffDoesNothing();
    testAlerts();
    testWebhook();
    testMqtt();
    testConfigChange();
    return testSummary("test_s4_outputs");
}
