// Tests for net/webhook.hpp and net/webhook_http.hpp: URL refusal, JSON payload, a bounded queue that drops the oldest and never blocks
// the caller, retries with back-off, time-outs, and a REAL plain-HTTP transport run against loopback stand-ins that are slow, close
// early, answer garbage, return errors, redirect, and come up late. No outside service is contacted.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/webhook.hpp"
#include "net/webhook_http.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

#include "netmap_stand_in.hpp"
#include "test_check.hpp"

namespace wh = cascade::net::webhook;
using Clock = std::chrono::steady_clock;

static long long msSince(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

// A transport whose behaviour the test scripts, with an optional gate that holds the worker inside post().
class FakeTransport : public wh::Transport {
public:
    bool https = false;
    std::function<wh::PostResult(int, const std::string&)> fn;
    bool supportsHttps() const override { return https; }
    wh::PostResult post(const wh::ParsedUrl& url, const std::string& body, const std::string& ct, int, int) override {
        int idx;
        {
            std::unique_lock<std::mutex> lk(m);
            idx = static_cast<int>(bodies.size());
            bodies.push_back(body);
            urls.push_back(url.scheme + "://" + url.host + ":" + std::to_string(url.port) + url.path);
            types.push_back(ct);
            times.push_back(Clock::now());
            cv.wait(lk, [&] { return !gated; });
        }
        if (fn) return fn(idx, body);
        wh::PostResult r;
        r.kind = wh::PostResult::Kind::Ok;
        r.status = 200;
        return r;
    }
    void gate(bool on) {
        std::lock_guard<std::mutex> lk(m);
        gated = on;
        cv.notify_all();
    }
    std::size_t calls() {
        std::lock_guard<std::mutex> lk(m);
        return bodies.size();
    }
    std::mutex m;
    std::condition_variable cv;
    bool gated = false;
    std::vector<std::string> bodies, urls, types;
    std::vector<Clock::time_point> times;
};

static wh::PostResult http(int status) {
    wh::PostResult r;
    r.kind = (status >= 200 && status < 300) ? wh::PostResult::Kind::Ok : wh::PostResult::Kind::HttpError;
    r.status = status;
    return r;
}

template <class F>
static bool waitFor(F f, int ms = 8000) {
    const auto t0 = Clock::now();
    while (msSince(t0) < ms) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

static wh::Config cfg(const std::string& url) {
    wh::Config c;
    c.url = url;
    c.backoffBaseMs = 30;
    c.backoffMaxMs = 400;
    c.connectTimeoutMs = 500;
    c.ioTimeoutMs = 300;
    return c;
}

int main() {
    // --- URL refusal ---------------------------------------------------------------------------------------------------------------------
    {
        wh::ParsedUrl u;
        CHECK(wh::parseUrl("http://example.org/hook", false, u) == wh::UrlError::None);
        CHECK(u.scheme == "http" && u.host == "example.org" && u.port == 80 && u.path == "/hook");
        CHECK(wh::parseUrl("HTTP://Example.org:8080/a/b?x=1&y=2", false, u) == wh::UrlError::None);
        CHECK(u.port == 8080 && u.path == "/a/b?x=1&y=2" && u.host == "Example.org");
        CHECK(wh::parseUrl("http://127.0.0.1:1", false, u) == wh::UrlError::None && u.path == "/" && u.port == 1);
        CHECK(wh::parseUrl("http://[::1]:9000/x", false, u) == wh::UrlError::None && u.host == "::1" && u.port == 9000);
        CHECK(wh::parseUrl("http://[::1]/x", false, u) == wh::UrlError::None && u.port == 80);
        CHECK(wh::parseUrl("http://h?x=1", false, u) == wh::UrlError::None && u.path == "/?x=1");
        CHECK(wh::parseUrl("http://h/p#frag", false, u) == wh::UrlError::None && u.path == "/p");     // a fragment is never sent
        CHECK(wh::parseUrl("http://h:65535/", false, u) == wh::UrlError::None && u.port == 65535);
        CHECK(wh::parseUrl("", false, u) == wh::UrlError::Empty);
        CHECK(wh::parseUrl("https://example.org/", false, u) == wh::UrlError::HttpsUnsupported);        // no TLS transport: refused
        CHECK(wh::parseUrl("HTTPS://example.org/", false, u) == wh::UrlError::HttpsUnsupported);
        CHECK(wh::parseUrl("https://example.org:8443/x", true, u) == wh::UrlError::None && u.scheme == "https" && u.port == 8443);   // allowed only with a TLS transport
        for (const char* bad : {"ftp://h/x", "file:///etc/passwd", "javascript:alert(1)", "data:text/plain,hi", "ws://h/", "gopher://h", "//h/x", "h/x",
                                "http:h", "http:/h", "htp://h", " http://h/", "mailto:a@b.c", "chrome://x", "view-source:http://h/"})
            CHECK(wh::parseUrl(bad, false, u) == wh::UrlError::BadScheme || wh::parseUrl(bad, false, u) == wh::UrlError::BadChars || wh::parseUrl(bad, false, u) == wh::UrlError::NoHost);
        CHECK(wh::parseUrl("ftp://h/x", true, u) == wh::UrlError::BadScheme);
        CHECK(wh::parseUrl("http://user:pass@h/x", false, u) == wh::UrlError::HasUserinfo);
        CHECK(wh::parseUrl("http://user@h/x", false, u) == wh::UrlError::HasUserinfo);
        CHECK(wh::parseUrl("http://h@evil/x", false, u) == wh::UrlError::HasUserinfo);
        CHECK(wh::parseUrl("http:///x", false, u) == wh::UrlError::NoHost);
        CHECK(wh::parseUrl("http://:80/x", false, u) == wh::UrlError::NoHost);
        CHECK(wh::parseUrl("http://h:0/x", false, u) == wh::UrlError::BadPort);
        CHECK(wh::parseUrl("http://h:65536/x", false, u) == wh::UrlError::BadPort);
        CHECK(wh::parseUrl("http://h:99999999/x", false, u) == wh::UrlError::BadPort);
        CHECK(wh::parseUrl("http://h:abc/x", false, u) == wh::UrlError::BadPort);
        CHECK(wh::parseUrl("http://h:/x", false, u) == wh::UrlError::BadPort);
        CHECK(wh::parseUrl("http://[::1]:/x", false, u) == wh::UrlError::BadPort);
        CHECK(wh::parseUrl("http://[::1/x", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://[::1]x/x", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://[]/x", false, u) == wh::UrlError::NoHost);
        CHECK(wh::parseUrl("http://[zz::1]/x", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://h h/x", false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl("http://h/\nGET /evil", false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl("http://h/\r\n", false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl(std::string("http://h/\0x", 11), false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl("http://h/\xC3\xA9", false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl("http://h\xC3\xA9/", false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl("http://h/a\\b", false, u) == wh::UrlError::BadChars);
        CHECK(wh::parseUrl("http://-h/", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://.h/", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://h..x/", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://h_x/", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://" + std::string(300, 'a') + "/", false, u) == wh::UrlError::BadHost);
        CHECK(wh::parseUrl("http://h/" + std::string(2100, 'a'), false, u) == wh::UrlError::TooLong);
        CHECK(std::string(wh::urlErrorText(wh::UrlError::HasUserinfo)).size() > 5);
    }

    // --- disabled by default; start refuses bad URLs; https refused without TLS --------------------------------------------------------------
    {
        FakeTransport ft;
        wh::WebhookSender off(ft, wh::Config{});
        CHECK(off.start() == wh::StartResult::Disabled);
        CHECK(!off.enqueue(wh::WebhookEvent{}) && !off.running());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(ft.calls() == 0);                                         // nothing was ever sent
        wh::UrlError why = wh::UrlError::None;
        wh::WebhookSender https(ft, cfg("https://example.org/h"));
        CHECK(https.start(&why) == wh::StartResult::HttpsUnsupported && why == wh::UrlError::HttpsUnsupported);
        CHECK(!https.running());
        wh::WebhookSender ftp(ft, cfg("ftp://example.org/h"));
        CHECK(ftp.start(&why) == wh::StartResult::BadUrl && why == wh::UrlError::BadScheme);
        wh::WebhookSender cred(ft, cfg("http://a:b@example.org/h"));
        CHECK(cred.start() == wh::StartResult::BadUrl);
        CHECK(!cred.enqueueJson("{}"));
        wh::HttpTransport real;
        CHECK(!real.supportsHttps());
        wh::WebhookSender httpsReal(real, cfg("https://example.org/h"));
        CHECK(httpsReal.start() == wh::StartResult::HttpsUnsupported);
        // with a transport that does speak TLS an https URL is accepted
        FakeTransport tls;
        tls.https = true;
        wh::WebhookSender ok(tls, cfg("https://example.org:8443/h"));
        CHECK(ok.start() == wh::StartResult::Started);
        CHECK(ok.start() == wh::StartResult::AlreadyRunning);
        CHECK(ok.enqueueJson("{\"a\":1}"));
        CHECK(waitFor([&] { return tls.calls() == 1; }));
        CHECK(tls.urls[0] == "https://example.org:8443/h" && tls.types[0] == "application/json");
        ok.stop();
        CHECK(!ok.enqueueJson("{}"));                                   // after stop nothing is accepted
    }

    // --- the payload ---------------------------------------------------------------------------------------------------------------------------
    {
        wh::WebhookEvent e;
        e.kind = "alert";
        e.title = "Signal above -40 dBFS";
        e.detail = "145.500 MHz";
        e.unixTime = 1700000000.5;
        e.freqHz = 145500000.0;
        e.source = "rule:strong";
        const auto j = nlohmann::json::parse(wh::eventToJson(e));
        CHECK(j["app"] == "FoxSDR" && j["kind"] == "alert" && j["title"] == "Signal above -40 dBFS" && j["detail"] == "145.500 MHz");
        CHECK(j["time"] == 1700000000.5 && j["freq_hz"] == 145500000.0 && j["source"] == "rule:strong");
        // hostile text: invalid UTF-8 and controls are replaced, never throw; quotes and newlines are escaped by the serialiser
        wh::WebhookEvent h;
        h.kind = "a\"b\n\x01\xff";
        h.title = std::string("nul\0x", 5) + "\xC3\x28";
        h.detail = "\"}]},{\"evil\":true,\"x\":\"";
        h.unixTime = std::nan("");
        h.freqHz = INFINITY;
        std::string s;
        bool threw = false;
        try { s = wh::eventToJson(h); } catch (...) { threw = true; }
        CHECK(!threw);
        const auto jh = nlohmann::json::parse(s);
        CHECK(!jh.contains("evil") && jh["time"] == 0.0 && jh["freq_hz"] == 0.0);
        CHECK(jh["detail"] == h.detail);
        // size: a megabyte of text, and a megabyte of characters that each need an escape, both fit the cap with the structure intact
        wh::WebhookEvent big;
        big.kind = "decode";
        big.title = "t";
        big.detail = std::string(1 << 20, 'x');
        std::string bs = wh::eventToJson(big);
        CHECK(bs.size() <= 16384 && nlohmann::json::parse(bs)["kind"] == "decode");
        big.detail = std::string(1 << 20, '"');
        bs = wh::eventToJson(big);
        CHECK(bs.size() <= 16384 && nlohmann::json::parse(bs)["title"] == "t");
        CHECK(wh::eventToJson(big, 300).size() <= 300);
        // an enqueue of an oversized raw body is rejected and counted
        FakeTransport ft;
        wh::Config c = cfg("http://127.0.0.1:9/h");
        c.maxBodyBytes = 1000;
        wh::WebhookSender sender(ft, c);
        CHECK(sender.start() == wh::StartResult::Started);
        CHECK(!sender.enqueueJson(std::string(1001, 'x')) && sender.stats().rejected == 1);
        CHECK(sender.enqueueJson(std::string(1000, 'x')));
    }

    // --- real HTTP: delivery, headers, body ----------------------------------------------------------------------------------------------------
    {
        netmap::HttpStandIn srv;
        CHECK(srv.start());
        wh::HttpTransport real;
        wh::WebhookSender sender(real, cfg(srv.url("/hook?token=abc")));
        CHECK(sender.start() == wh::StartResult::Started);
        wh::WebhookEvent e;
        e.kind = "decode";
        e.title = "ADS-B 4CA7B2";
        e.freqHz = 1090e6;
        CHECK(sender.enqueue(e));
        CHECK(waitFor([&] { return sender.stats().delivered == 1; }));
        const auto rq = srv.requests();
        CHECK(rq.size() == 1 && rq[0].method == "POST" && rq[0].path == "/hook");
        CHECK(rq[0].contentType == "application/json" && rq[0].userAgent == "FoxSDR-webhook/1");
        CHECK(nlohmann::json::parse(rq[0].body)["title"] == "ADS-B 4CA7B2");
        const auto st = sender.stats();
        CHECK(st.attempts == 1 && st.retries == 0 && st.gaveUp == 0 && st.lastStatus == 200);
        // order is kept
        for (int i = 0; i < 20; ++i) { wh::WebhookEvent x; x.kind = "n"; x.title = std::to_string(i); sender.enqueue(x); }
        CHECK(waitFor([&] { return sender.stats().delivered == 21; }));
        const auto rq2 = srv.requests();
        bool inOrder = rq2.size() == 21;
        for (int i = 0; inOrder && i < 20; ++i) inOrder = nlohmann::json::parse(rq2[static_cast<std::size_t>(i) + 1].body)["title"] == std::to_string(i);
        CHECK(inOrder);
    }

    // --- retries with back-off ---------------------------------------------------------------------------------------------------------------------
    {
        netmap::HttpStandIn srv;
        CHECK(srv.start([](int idx, const httplib::Request&, httplib::Response& res) { res.status = idx < 2 ? 500 : 200; res.set_content("x", "text/plain"); }));
        wh::HttpTransport real;
        wh::Config c = cfg(srv.url());
        c.backoffBaseMs = 80;
        c.backoffMaxMs = 1000;
        wh::WebhookSender sender(real, c);
        CHECK(sender.start() == wh::StartResult::Started);
        const auto t0 = Clock::now();
        sender.enqueue(wh::WebhookEvent{});
        CHECK(waitFor([&] { return sender.stats().delivered == 1; }));
        const auto rq = srv.requests();
        CHECK(rq.size() == 3);
        const long long gap1 = std::chrono::duration_cast<std::chrono::milliseconds>(rq[1].at - rq[0].at).count();
        const long long gap2 = std::chrono::duration_cast<std::chrono::milliseconds>(rq[2].at - rq[1].at).count();
        std::printf("INFO back-off gaps %lld ms (base 80) and %lld ms (expected about 160); total %lld ms\n", gap1, gap2, msSince(t0));
        CHECK(gap1 >= 70 && gap2 >= 150);                                // doubling
        CHECK(gap2 > gap1);
        const auto st = sender.stats();
        CHECK(st.retries == 2 && st.attempts == 3 && st.delivered == 1 && st.gaveUp == 0);
    }
    {
        // the cap on the delay
        FakeTransport ft;
        ft.fn = [](int, const std::string&) { return http(503); };
        wh::Config c = cfg("http://127.0.0.1:9/h");
        c.maxAttempts = 6;
        c.backoffBaseMs = 20;
        c.backoffMaxMs = 50;
        wh::WebhookSender sender(ft, c);
        sender.start();
        sender.enqueueJson("{}");
        CHECK(waitFor([&] { return sender.stats().gaveUp == 1; }));
        // waits: 20, 40, 50, 50, 50 (capped)
        const long long total = std::chrono::duration_cast<std::chrono::milliseconds>(ft.times.back() - ft.times.front()).count();
        std::printf("INFO six attempts with base 20 ms and cap 50 ms took %lld ms (expected about 210)\n", total);
        CHECK(ft.calls() == 6 && total >= 190 && total < 1500);
    }

    // --- which statuses retry ------------------------------------------------------------------------------------------------------------------------
    {
        struct Row { int status; bool retried; };
        for (Row row : {Row{500, true}, Row{502, true}, Row{503, true}, Row{599, true}, Row{408, true}, Row{429, true}, Row{400, false}, Row{401, false},
                        Row{403, false}, Row{404, false}, Row{410, false}, Row{413, false}, Row{301, false}, Row{302, false}, Row{307, false}, Row{100, false}, Row{199, false}}) {
            FakeTransport ft;
            ft.fn = [&](int, const std::string&) { return http(row.status); };
            wh::Config c = cfg("http://127.0.0.1:9/h");
            c.maxAttempts = 3;
            c.backoffBaseMs = 5;
            wh::WebhookSender sender(ft, c);
            sender.start();
            sender.enqueueJson("{}");
            CHECK(waitFor([&] { return sender.stats().gaveUp + sender.stats().permanentFailures == 1; }));
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            CHECK(ft.calls() == (row.retried ? 3u : 1u));
            CHECK((sender.stats().gaveUp == 1) == row.retried && (sender.stats().permanentFailures == 1) == !row.retried);
        }
        FakeTransport ok;
        for (int s : {200, 201, 202, 204, 299}) {
            ok.fn = [s](int, const std::string&) { return http(s); };
            wh::WebhookSender sender(ok, cfg("http://127.0.0.1:9/h"));
            sender.start();
            sender.enqueueJson("{}");
            CHECK(waitFor([&] { return sender.stats().delivered == 1; }));
            CHECK(sender.stats().retries == 0);
        }
    }

    // --- real stand-in: error statuses and a redirect that must NOT be followed ---------------------------------------------------------------------
    {
        netmap::HttpStandIn srv;
        CHECK(srv.start([](int, const httplib::Request& req, httplib::Response& res) {
            if (req.path == "/hook") { res.status = 302; res.set_header("Location", "/landed"); }
            else if (req.path == "/gone") res.status = 410;
            else if (req.path == "/limited") res.status = 429;
            else res.status = 200;
        }));
        wh::HttpTransport real;
        {
            wh::WebhookSender s(real, cfg(srv.url("/hook")));
            s.start();
            s.enqueueJson("{\"a\":1}");
            CHECK(waitFor([&] { return s.stats().permanentFailures == 1; }));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(s.stats().lastStatus == 302 && s.stats().attempts == 1);   // not retried
            for (const auto& r : srv.requests()) CHECK(r.path != "/landed");  // and never followed
        }
        {
            wh::WebhookSender s(real, cfg(srv.url("/gone")));
            s.start();
            s.enqueueJson("{}");
            CHECK(waitFor([&] { return s.stats().permanentFailures == 1; }));
            CHECK(s.stats().lastStatus == 410 && s.stats().retries == 0);
        }
        {
            wh::Config c = cfg(srv.url("/limited"));
            c.maxAttempts = 3;
            c.backoffBaseMs = 10;
            wh::WebhookSender s(real, c);
            s.start();
            s.enqueueJson("{}");
            CHECK(waitFor([&] { return s.stats().gaveUp == 1; }));
            CHECK(s.stats().attempts == 3 && s.stats().lastStatus == 429);
        }
    }

    // --- a slow receiver: time-outs, and the caller never waits ---------------------------------------------------------------------------------------
    {
        netmap::HttpStandIn srv;
        CHECK(srv.start([](int, const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            res.status = 200;
            res.set_content("late", "text/plain");
        }));
        wh::HttpTransport real;
        wh::Config c = cfg(srv.url());
        c.ioTimeoutMs = 150;
        c.maxAttempts = 2;
        c.backoffBaseMs = 20;
        wh::WebhookSender sender(real, c);
        sender.start();
        const auto t0 = Clock::now();
        sender.enqueueJson("{}");
        // while the worker is stuck waiting for the slow answer, a thousand more events are queued: none of the calls may wait
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        long long worst = 0;
        const auto tq = Clock::now();
        for (int i = 0; i < 1000; ++i) {
            const auto a = Clock::now();
            sender.enqueueJson("{\"i\":" + std::to_string(i) + "}");
            worst = std::max(worst, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - a).count());
        }
        const long long all = msSince(tq);
        std::printf("INFO 1000 enqueues while the worker waited on a slow receiver: %lld ms in total, worst single call %lld us\n", all, worst);
        CHECK(all < 400 && worst < 100000);
        CHECK(waitFor([&] { return sender.stats().gaveUp >= 1; }));
        const auto st = sender.stats();
        CHECK(st.timeouts >= 2 && st.delivered == 0);
        const long long firstEvent = msSince(t0);
        CHECK(st.droppedOldest == 900);                                  // 1000 queued into 100 slots: the oldest 900 were dropped
        CHECK(sender.stats().queued <= 100);
        std::printf("INFO first event gave up after %lld ms (2 attempts of 150 ms plus back-off)\n", firstEvent);
        sender.stop();
        srv.stop();
    }

    // --- a receiver that closes early, answers garbage, never answers, or is not there --------------------------------------------------------------------
    {
        wh::HttpTransport real;
        using Mode = netmap::RawStandIn::Mode;
        for (Mode mode : {Mode::CloseImmediately, Mode::ReadRequestThenClose, Mode::GarbageThenClose, Mode::Blackhole}) {
            netmap::RawStandIn raw(mode);
            CHECK(raw.start());
            wh::Config c = cfg(raw.url());
            c.ioTimeoutMs = 200;
            c.maxAttempts = 3;
            c.backoffBaseMs = 20;
            wh::WebhookSender sender(real, c);
            sender.start();
            const auto t0 = Clock::now();
            sender.enqueueJson("{\"hello\":1}");
            CHECK(waitFor([&] { return sender.stats().gaveUp == 1; }));
            const auto st = sender.stats();
            std::printf("INFO raw stand-in mode %d: %llu attempts, %llu network errors, %llu time-outs, gave up after %lld ms, %d connections\n",
                        static_cast<int>(mode), static_cast<unsigned long long>(st.attempts), static_cast<unsigned long long>(st.networkErrors),
                        static_cast<unsigned long long>(st.timeouts), msSince(t0), raw.accepted());
            CHECK(st.attempts == 3 && st.retries == 2 && st.delivered == 0 && st.permanentFailures == 0);
            CHECK(st.networkErrors + st.timeouts == 3);
            if (mode == Mode::Blackhole) CHECK(st.timeouts == 3);
            CHECK(raw.accepted() == 3);
            sender.stop();
        }
        // nothing listening: connection refused
        wh::Config c = cfg("http://127.0.0.1:" + std::to_string(netmap::unusedPort()) + "/hook");
        c.maxAttempts = 2;
        c.backoffBaseMs = 10;
        wh::WebhookSender sender(real, c);
        sender.start();
        sender.enqueueJson("{}");
        CHECK(waitFor([&] { return sender.stats().gaveUp == 1; }));
        CHECK(sender.stats().networkErrors + sender.stats().timeouts == 2);
    }

    // --- the receiver comes up late: the event is delivered by a retry --------------------------------------------------------------------------------------
    {
        const int port = netmap::unusedPort();
        wh::HttpTransport real;
        wh::Config c = cfg("http://127.0.0.1:" + std::to_string(port) + "/hook");
        c.maxAttempts = 8;
        c.backoffBaseMs = 60;
        c.backoffMaxMs = 120;
        wh::WebhookSender sender(real, c);
        sender.start();
        sender.enqueueJson("{\"late\":true}");
        CHECK(waitFor([&] { return sender.stats().attempts >= 2; }));
        netmap::HttpStandIn srv(port);
        if (srv.start() && srv.port() == port) {
            CHECK(waitFor([&] { return sender.stats().delivered == 1; }));
            CHECK(sender.stats().retries >= 1 && srv.count() == 1);
            CHECK(nlohmann::json::parse(srv.requests()[0].body)["late"] == true);
        } else {
            std::printf("SKIP late-receiver delivery: could not bind the chosen port\n");
        }
    }

    // --- the queue: bounded, oldest dropped, newest kept, in order -------------------------------------------------------------------------------------------
    {
        FakeTransport ft;
        ft.gated = true;       // the worker will sit inside the first post()
        wh::Config c = cfg("http://127.0.0.1:9/h");
        c.maxQueue = 10;
        wh::WebhookSender sender(ft, c);
        sender.start();
        sender.enqueueJson("{\"n\":0}");
        CHECK(waitFor([&] { return ft.calls() == 1; }));   // event 0 is in flight
        for (int i = 1; i <= 500; ++i) sender.enqueueJson("{\"n\":" + std::to_string(i) + "}");
        auto st = sender.stats();
        CHECK(st.queued == 10 && st.droppedOldest == 490 && st.enqueued == 501);
        ft.gate(false);
        CHECK(waitFor([&] { return sender.stats().delivered == 11; }));
        CHECK(ft.bodies[0] == "{\"n\":0}");
        bool newest = true;
        for (int k = 0; k < 10; ++k) newest = newest && ft.bodies[static_cast<std::size_t>(k) + 1] == "{\"n\":" + std::to_string(491 + k) + "}";
        CHECK(newest);                                       // events 491..500, in order: the oldest were the ones lost
        CHECK(sender.stats().queued == 0);
    }

    // --- stop() is prompt, even in a long back-off; destruction is safe -----------------------------------------------------------------------------------------
    {
        FakeTransport ft;
        ft.fn = [](int, const std::string&) { return http(500); };
        wh::Config c = cfg("http://127.0.0.1:9/h");
        c.backoffBaseMs = 5000;
        c.backoffMaxMs = 60000;
        auto sender = std::make_unique<wh::WebhookSender>(ft, c);
        sender->start();
        sender->enqueueJson("{}");
        CHECK(waitFor([&] { return sender->stats().retries == 1; }));       // now waiting 5 s
        const auto t0 = Clock::now();
        sender->stop();
        const long long took = msSince(t0);
        std::printf("INFO stop() during a 5000 ms back-off took %lld ms\n", took);
        CHECK(took < 500);
        CHECK(!sender->enqueueJson("{}"));
        sender.reset();
        // restart after stop works
        wh::WebhookSender again(ft, cfg("http://127.0.0.1:9/h"));
        CHECK(again.start() == wh::StartResult::Started);
        again.stop();
        again.stop();                                         // twice is fine
        CHECK(again.start() == wh::StartResult::Started);
        // destroyed while running and while events are queued
        auto s2 = std::make_unique<wh::WebhookSender>(ft, cfg("http://127.0.0.1:9/h"));
        s2->start();
        for (int i = 0; i < 50; ++i) s2->enqueueJson("{}");
        s2.reset();
        CHECK(true);
    }

    // --- many threads enqueue; every event is accounted for -------------------------------------------------------------------------------------------------------
    {
        FakeTransport ft;
        wh::Config c = cfg("http://127.0.0.1:9/h");
        c.maxQueue = 50;
        wh::WebhookSender sender(ft, c);
        sender.start();
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&] { for (int i = 0; i < 2000; ++i) sender.enqueueJson("{}"); });
        for (auto& t : th) t.join();
        CHECK(waitFor([&] { const auto s = sender.stats(); return s.queued == 0 && s.delivered + s.droppedOldest == 8000; }));
        const auto st = sender.stats();
        std::printf("INFO 4 threads x 2000 events into a 50-slot queue: %llu delivered, %llu dropped, none lost\n",
                    static_cast<unsigned long long>(st.delivered), static_cast<unsigned long long>(st.droppedOldest));
        CHECK(st.enqueued == 8000 && st.delivered + st.droppedOldest == 8000 && st.gaveUp == 0);
    }

    return testSummary("test_webhook");
}
