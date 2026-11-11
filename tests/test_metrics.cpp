// Tests for net/metrics.hpp: the Prometheus text exposition format 0.0.4 writer, its bounds and its HTTP handler. The output is
// checked against hand-written golden text AND against an independent parser written here from the format document (so a mistake in
// the writer that the golden text does not cover - an unescaped quote, a split group - still fails). A real loopback HTTP server
// serves the handler to a real client.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/metrics.hpp"

#include <atomic>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include "test_check.hpp"

namespace m = cascade::net::metrics;

// ---- an independent reader of the format ------------------------------------------------------------------------------------------
struct Parsed {
    bool ok = true;
    std::string why;
    std::map<std::string, std::string> type, help;
    std::map<std::string, double> sampleCount;   // by family name
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> samples;
    std::vector<double> values;
};

static bool validUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

static Parsed parse(const std::string& text) {
    Parsed p;
    auto fail = [&](const std::string& w) { if (p.ok) { p.ok = false; p.why = w; } };
    if (text.empty() || text.back() != '\n') fail("no final line feed");
    if (!validUtf8(text)) fail("invalid utf-8");
    std::istringstream in(text);
    std::string line, current;
    std::set<std::string> closed, seen;
    std::set<std::string> uniq;
    auto nameOk = [](const std::string& n, bool metric) {
        if (n.empty()) return false;
        for (std::size_t i = 0; i < n.size(); ++i) {
            char c = n[i];
            bool a = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (metric && c == ':');
            bool d = c >= '0' && c <= '9';
            if (!(a || (i && d))) return false;
        }
        return true;
    };
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line.rfind("# HELP ", 0) == 0) {
            const std::string rest = line.substr(7);
            const std::size_t sp = rest.find(' ');
            const std::string name = rest.substr(0, sp);
            if (!nameOk(name, true)) fail("bad HELP name");
            if (p.help.count(name)) fail("two HELP lines for " + name);
            if (seen.count(name)) fail("HELP after samples of " + name);
            const std::string doc = sp == std::string::npos ? "" : rest.substr(sp + 1);
            // the only escapes allowed are \\ and \n
            for (std::size_t i = 0; i < doc.size(); ++i)
                if (doc[i] == '\\') { if (i + 1 >= doc.size() || (doc[i + 1] != '\\' && doc[i + 1] != 'n')) fail("bad HELP escape"); else ++i; }
            p.help[name] = doc;
            if (current != name) { if (!current.empty()) closed.insert(current); current = name; }
            if (closed.count(name)) fail("group of " + name + " split");
            continue;
        }
        if (line.rfind("# TYPE ", 0) == 0) {
            std::istringstream ls(line.substr(7));
            std::string name, type;
            ls >> name >> type;
            if (!nameOk(name, true)) fail("bad TYPE name");
            if (type != "counter" && type != "gauge" && type != "untyped" && type != "histogram" && type != "summary") fail("bad TYPE");
            if (p.type.count(name)) fail("two TYPE lines for " + name);
            if (seen.count(name)) fail("TYPE after samples of " + name);
            p.type[name] = type;
            if (current != name) { if (!current.empty()) closed.insert(current); current = name; }
            if (closed.count(name)) fail("group of " + name + " split");
            continue;
        }
        if (line[0] == '#') continue;
        // a sample
        std::size_t i = 0;
        while (i < line.size() && line[i] != '{' && line[i] != ' ') ++i;
        const std::string name = line.substr(0, i);
        if (!nameOk(name, true)) fail("bad sample name '" + name + "'");
        std::map<std::string, std::string> labels;
        if (i < line.size() && line[i] == '{') {
            ++i;
            while (true) {
                std::size_t eq = line.find('=', i);
                if (eq == std::string::npos) { fail("label without ="); break; }
                const std::string ln = line.substr(i, eq - i);
                if (!nameOk(ln, false) || ln.rfind("__", 0) == 0) fail("bad label name '" + ln + "'");
                if (eq + 1 >= line.size() || line[eq + 1] != '"') { fail("label value not quoted"); break; }
                std::string val;
                std::size_t j = eq + 2;
                bool closedQuote = false;
                while (j < line.size()) {
                    if (line[j] == '\\') {
                        if (j + 1 >= line.size()) { fail("dangling backslash"); break; }
                        const char e = line[j + 1];
                        if (e == '\\') val += '\\'; else if (e == '"') val += '"'; else if (e == 'n') val += '\n'; else fail("bad label escape");
                        j += 2;
                    } else if (line[j] == '"') { closedQuote = true; ++j; break; }
                    else val += line[j++];
                }
                if (!closedQuote) { fail("unterminated label value"); break; }
                if (labels.count(ln)) fail("duplicate label");
                labels[ln] = val;
                if (j < line.size() && line[j] == ',') { i = j + 1; continue; }
                if (j < line.size() && line[j] == '}') { i = j + 1; break; }
                fail("bad label list");
                break;
            }
        }
        if (i >= line.size() || line[i] != ' ') { fail("no value separator"); continue; }
        const std::string vs = line.substr(i + 1);
        double v = 0;
        if (vs == "NaN") v = std::nan("");
        else if (vs == "+Inf") v = INFINITY;
        else if (vs == "-Inf") v = -INFINITY;
        else {
            char* end = nullptr;
            v = std::strtod(vs.c_str(), &end);
            if (vs.empty() || end != vs.c_str() + vs.size()) fail("bad value '" + vs + "'");
            for (char c : vs) if (c == ' ') fail("timestamp or space in value");
        }
        if (!p.type.count(name)) fail("sample before TYPE: " + name);
        if (current != name) { if (!current.empty()) closed.insert(current); current = name; }
        if (closed.count(name)) fail("group of " + name + " split");
        seen.insert(name);
        std::string id = name;
        for (auto& l : labels) id += '\x1f' + l.first + '=' + l.second;
        if (!uniq.insert(id).second) fail("duplicate series " + name);
        p.samples.push_back({name, labels});
        p.values.push_back(v);
        p.sampleCount[name] += 1;
    }
    return p;
}

static std::string findValue(const Parsed& p, const std::string& name, const std::map<std::string, std::string>& labels) {
    for (std::size_t i = 0; i < p.samples.size(); ++i)
        if (p.samples[i].first == name && p.samples[i].second == labels) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", p.values[i]);
            return buf;
        }
    return "";
}

int main() {
    // --- golden text -------------------------------------------------------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        CHECK(r.define("foxsdr_decodes_total", m::MetricType::Counter, "Decoded messages."));
        CHECK(r.define("foxsdr_level_dbfs", m::MetricType::Gauge, "Level."));
        CHECK(r.inc("foxsdr_decodes_total", {{"decoder", "ads-b"}}, 3) == m::Result::Ok);
        CHECK(r.inc("foxsdr_decodes_total", {{"decoder", "ais"}}) == m::Result::Ok);
        CHECK(r.set("foxsdr_level_dbfs", {}, -42.5) == m::Result::Ok);
        const std::string expect =
            "# HELP foxsdr_decodes_total Decoded messages.\n"
            "# TYPE foxsdr_decodes_total counter\n"
            "foxsdr_decodes_total{decoder=\"ads-b\"} 3\n"
            "foxsdr_decodes_total{decoder=\"ais\"} 1\n"
            "# HELP foxsdr_level_dbfs Level.\n"
            "# TYPE foxsdr_level_dbfs gauge\n"
            "foxsdr_level_dbfs -42.5\n"
            "# HELP foxsdr_metrics_dropped_series_total Samples dropped because a label set exceeded a cardinality bound.\n"
            "# TYPE foxsdr_metrics_dropped_series_total counter\n"
            "foxsdr_metrics_dropped_series_total 0\n";
        CHECK(r.render() == expect);
        const Parsed p = parse(r.render());
        CHECK(p.ok);
        if (!p.ok) std::printf("parse: %s\n", p.why.c_str());
        // an empty registry still renders valid text
        m::MetricsRegistry e;
        CHECK(parse(e.render()).ok);
    }

    // --- escaping ------------------------------------------------------------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        r.define("t_total", m::MetricType::Counter, "back\\slash and\nnewline");
        r.inc("t_total", {{"d", "a\"b\\c\nd"}});
        const std::string text = r.render();
        CHECK(text.find("# HELP t_total back\\\\slash and\\nnewline\n") != std::string::npos);
        CHECK(text.find("t_total{d=\"a\\\"b\\\\c\\nd\"} 1\n") != std::string::npos);
        const Parsed p = parse(text);
        CHECK(p.ok);
        CHECK(p.samples.size() == 2 && p.samples[0].second.at("d") == "a\"b\\c\nd");   // the independent reader recovers the original
        // UTF-8 kept, invalid bytes and control bytes replaced, nothing can inject a line
        m::MetricsRegistry u;
        u.define("u", m::MetricType::Gauge, "x");
        u.set("u", {{"v", "caf\xC3\xA9 \xE4\xB8\x96"}}, 1);
        u.set("u", {{"v", "bad\xFF\xFE" "x"}}, 2);
        u.set("u", {{"v", std::string("nul\0byte", 8)}}, 3);
        u.set("u", {{"v", "overlong\xC0\xAF"}}, 4);
        u.set("u", {{"v", "surrogate\xED\xA0\x80"}}, 5);
        u.set("u", {{"v", "toobig\xF4\x90\x80\x80"}}, 6);
        u.set("u", {{"v", "cut\xE4\xB8"}}, 7);
        u.set("u", {{"v", "x\"}\nevil_metric 99\n#"}}, 8);
        u.set("u", {{"v", "bell\x07tab\t"}}, 9);
        const std::string t = u.render();
        const Parsed pu = parse(t);
        CHECK(pu.ok);
        if (!pu.ok) std::printf("parse: %s\n", pu.why.c_str());
        CHECK(t.find("evil_metric 99\n") == std::string::npos);
        CHECK(t.find("caf\xC3\xA9 \xE4\xB8\x96") != std::string::npos);
        CHECK(t.find("bad\xEF\xBF\xBD\xEF\xBF\xBD" "x") != std::string::npos);
        CHECK(pu.sampleCount.at("u") == 9);
        // a long value is cut on a character boundary at 128 bytes
        m::MetricsRegistry c;
        c.define("c", m::MetricType::Gauge, "");
        std::string longv;
        for (int i = 0; i < 100; ++i) longv += "\xC3\xA9";   // 200 bytes of two-byte characters
        c.set("c", {{"v", longv}}, 1);
        const Parsed pc = parse(c.render());
        CHECK(pc.ok && pc.samples[0].second.at("v").size() == 128 && validUtf8(pc.samples[0].second.at("v")));
    }

    // --- value formats ------------------------------------------------------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        r.define("g", m::MetricType::Gauge, "g");
        const double vals[] = {0.0, -0.0, 1.0, -1.0, 123456789012.0, 1e15, 1e300, 0.1, -42.5, 1.0 / 3.0, 5e-324, std::nan(""), INFINITY, -INFINITY};
        int i = 0;
        for (double v : vals) r.set("g", {{"i", std::to_string(i++)}}, v);
        const Parsed p = parse(r.render());
        CHECK(p.ok);
        if (!p.ok) std::printf("parse: %s\n", p.why.c_str());
        i = 0;
        for (double v : vals) {
            double got = -1;
            for (std::size_t k = 0; k < p.samples.size(); ++k)
                if (p.samples[k].first == "g" && p.samples[k].second.at("i") == std::to_string(i)) got = p.values[k];
            if (std::isnan(v)) CHECK(std::isnan(got)); else CHECK(got == v);   // round trips exactly
            ++i;
        }
        const std::string t = r.render();
        CHECK(t.find("g{i=\"1\"} 0\n") != std::string::npos);             // negative zero is written as 0
        CHECK(t.find("g{i=\"11\"} NaN\n") != std::string::npos);
        CHECK(t.find("g{i=\"12\"} +Inf\n") != std::string::npos);
        CHECK(t.find("g{i=\"13\"} -Inf\n") != std::string::npos);
        CHECK(t.find("g{i=\"4\"} 123456789012\n") != std::string::npos);  // no exponent for whole numbers
    }

    // --- definitions, types and validation ------------------------------------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        CHECK(r.define("ok_name:x", m::MetricType::Gauge, "h"));
        CHECK(!r.define("ok_name:x", m::MetricType::Gauge, "h"));        // duplicate
        for (const char* bad : {"", "1abc", "a-b", "a b", "a.b", "a\"b", "é", "a\n"}) CHECK(!r.define(bad, m::MetricType::Gauge, ""));
        CHECK(!r.define(std::string(101, 'a'), m::MetricType::Gauge, ""));
        r.define("cnt_total", m::MetricType::Counter, "c");
        CHECK(r.set("cnt_total", {}, 1) == m::Result::WrongType);          // a counter is not set as a gauge
        CHECK(r.inc("ok_name:x") == m::Result::WrongType);
        CHECK(r.inc("nope") == m::Result::UnknownFamily);
        CHECK(r.inc("cnt_total", {}, -1) == m::Result::BadValue);
        CHECK(r.inc("cnt_total", {}, std::nan("")) == m::Result::BadValue);
        CHECK(r.inc("cnt_total", {}, INFINITY) == m::Result::BadValue);
        CHECK(r.setCounter("cnt_total", {}, -5) == m::Result::BadValue);
        CHECK(r.setCounter("cnt_total", {}, 100) == m::Result::Ok);
        CHECK(r.setCounter("cnt_total", {}, 7) == m::Result::Ok);           // a lower total is a reset (a restarted engine counter)
        CHECK(findValue(parse(r.render()), "cnt_total", {}) == "7");
        CHECK(r.add("ok_name:x", {}, 2.5) == m::Result::Ok && r.add("ok_name:x", {}, -0.5) == m::Result::Ok);
        CHECK(findValue(parse(r.render()), "ok_name:x", {}) == "2");
        CHECK(r.add("ok_name:x", {}, std::nan("")) == m::Result::BadValue);
        // label names
        for (const char* bad : {"", "1a", "a-b", "__reserved", "a b", "a\"", "é"}) CHECK(r.set("ok_name:x", {{bad, "v"}}, 1) == m::Result::BadLabels);
        CHECK(r.set("ok_name:x", {{"a", "1"}, {"a", "2"}}, 1) == m::Result::BadLabels);        // duplicate label name
        CHECK(r.set("ok_name:x", {{"a", "1"}, {"b", "1"}, {"c", "1"}, {"d", "1"}, {"e", "1"}}, 1) == m::Result::BadLabels);   // five labels
        CHECK(r.set("ok_name:x", {{"a", "1"}, {"b", "1"}, {"c", "1"}, {"d", "1"}}, 1) == m::Result::Ok);
        // label order does not matter: one series
        const std::size_t before = r.seriesCount();
        r.set("ok_name:x", {{"y", "1"}, {"x", "2"}}, 1);
        r.set("ok_name:x", {{"x", "2"}, {"y", "1"}}, 5);
        CHECK(r.seriesCount() == before + 1);
        const Parsed p = parse(r.render());
        CHECK(p.ok);
        CHECK(r.render().find("ok_name:x{x=\"2\",y=\"1\"} 5\n") != std::string::npos);        // sorted by label name
        // an empty label value is legal in the format
        CHECK(r.set("ok_name:x", {{"e", ""}}, 1) == m::Result::Ok);
        CHECK(parse(r.render()).ok);
        // many families: bounded
        m::MetricsRegistry many;
        int defined = 0;
        for (int i = 0; i < 200; ++i) if (many.define("f" + std::to_string(i), m::MetricType::Gauge, "")) ++defined;
        CHECK(defined == 64 && many.familyCount() == 64);
    }

    // --- cardinality bounds ----------------------------------------------------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        r.define("by_user_total", m::MetricType::Counter, "per user: the classic cardinality bomb");
        int ok = 0, limited = 0;
        for (int i = 0; i < 1000; ++i) {
            const auto res = r.inc("by_user_total", {{"user", "u" + std::to_string(i)}});
            if (res == m::Result::Ok) ++ok;
            if (res == m::Result::CardinalityLimit) ++limited;
        }
        CHECK(ok == 64 && limited == 936);
        CHECK(r.droppedSeries() == 936);
        CHECK(r.inc("by_user_total", {{"user", "u3"}}) == m::Result::Ok);       // an existing series still updates
        const Parsed p = parse(r.render());
        CHECK(p.ok);
        CHECK(p.sampleCount.at("by_user_total") == 64);
        CHECK(findValue(p, "foxsdr_metrics_dropped_series_total", {}) == "936");
        // the total bound across families
        m::Limits lim;
        lim.maxSeriesPerFamily = 100;
        lim.maxTotalSeries = 150;
        m::MetricsRegistry t(lim);
        t.define("a", m::MetricType::Gauge, "");
        t.define("b", m::MetricType::Gauge, "");
        for (int i = 0; i < 100; ++i) t.set("a", {{"i", std::to_string(i)}}, 1);
        int bOk = 0;
        for (int i = 0; i < 100; ++i) if (t.set("b", {{"i", std::to_string(i)}}, 1) == m::Result::Ok) ++bOk;
        CHECK(bOk == 50 && t.seriesCount() == 150);
        // random hostile content never produces text the independent reader rejects
        std::mt19937 rng(3);
        m::MetricsRegistry h;
        h.define("h", m::MetricType::Gauge, "x");
        for (int i = 0; i < 3000; ++i) {
            std::string v;
            const int n = static_cast<int>(rng() % 40);
            for (int k = 0; k < n; ++k) v += static_cast<char>(rng() % 256);
            h.set("h", {{"v", v}}, static_cast<double>(rng()) / 7.0);
        }
        const Parsed ph = parse(h.render());
        CHECK(ph.ok);
        if (!ph.ok) std::printf("parse: %s\n", ph.why.c_str());
    }

    // --- the handler -------------------------------------------------------------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        m::StandardMetrics std_(r);
        m::EngineSnapshot s;
        s.decodesByDecoder = {{"adsb", 1200}, {"ais", 34}, {"ft8", 0}};
        s.droppedSamples = 7;
        s.signalLevelDbfs = -61.25;
        s.dspCpuShare = 0.375;
        s.connectedClients = 2;
        std_.publish(s);
        const std::string text = r.render();
        const Parsed p = parse(text);
        CHECK(p.ok);
        CHECK(findValue(p, "foxsdr_decodes_total", {{"decoder", "adsb"}}) == "1200");
        CHECK(findValue(p, "foxsdr_decodes_total", {{"decoder", "ft8"}}) == "0");
        CHECK(findValue(p, "foxsdr_dropped_samples_total", {}) == "7");
        CHECK(findValue(p, "foxsdr_signal_level_dbfs", {}) == "-61.25");
        CHECK(findValue(p, "foxsdr_dsp_thread_cpu_ratio", {}) == "0.375");
        CHECK(findValue(p, "foxsdr_connected_clients", {}) == "2");
        // a later snapshot updates in place; out-of-range and non-finite values are handled
        s.decodesByDecoder["adsb"] = 1300;
        s.dspCpuShare = 7.0;                                 // clamped to 1
        s.signalLevelDbfs = std::nan("");                    // not published: the old value stays
        s.connectedClients = -3;
        std_.publish(s);
        const Parsed p2 = parse(r.render());
        CHECK(findValue(p2, "foxsdr_decodes_total", {{"decoder", "adsb"}}) == "1300");
        CHECK(findValue(p2, "foxsdr_dsp_thread_cpu_ratio", {}) == "1");
        CHECK(findValue(p2, "foxsdr_signal_level_dbfs", {}) == "-61.25");
        CHECK(findValue(p2, "foxsdr_connected_clients", {}) == "0");

        m::MetricsHandler h(r);
        auto g = h.handle("GET", "/metrics");
        CHECK(g.status == 200 && g.contentType == "text/plain; version=0.0.4; charset=utf-8" && g.body == r.render());
        CHECK(h.handle("GET", "/metrics?x=1&y=2").status == 200);
        auto hd = h.handle("HEAD", "/metrics");
        CHECK(hd.status == 200 && hd.body.empty() && hd.contentType == g.contentType);
        auto post = h.handle("POST", "/metrics");
        CHECK(post.status == 405 && post.headers.size() == 1 && post.headers[0].first == "Allow");
        CHECK(h.handle("DELETE", "/metrics").status == 405);
        CHECK(h.handle("GET", "/other").status == 404);
        CHECK(h.handle("GET", "/metrics/").status == 404);
        CHECK(h.handle("GET", "/METRICS").status == 404);
        CHECK(h.handle("GET", "").status == 404);

        m::HandlerConfig cfg;
        cfg.bearerToken = "s3cret-token";
        m::MetricsHandler hs(r, cfg);
        CHECK(hs.handle("GET", "/metrics").status == 401);
        CHECK(hs.handle("GET", "/metrics", "").status == 401);
        CHECK(hs.handle("GET", "/metrics", "Bearer wrong").status == 401);
        CHECK(hs.handle("GET", "/metrics", "Bearer s3cret-token ").status == 401);
        CHECK(hs.handle("GET", "/metrics", "Bearer s3cret-toke").status == 401);
        CHECK(hs.handle("GET", "/metrics", "Basic czNjcmV0LXRva2Vu").status == 401);
        CHECK(hs.handle("GET", "/metrics", "s3cret-token").status == 401);
        CHECK(hs.handle("GET", "/metrics", "Bearer " + std::string(10000, 'a')).status == 401);
        CHECK(hs.handle("GET", "/metrics", "Bearer s3cret-token").status == 200);
        CHECK(hs.handle("GET", "/metrics", "bearer s3cret-token").status == 200);
        CHECK(hs.handle("GET", "/metrics", "Bearer s3cret-token").body == r.render());
        CHECK(hs.handle("GET", "/metrics", "").headers[0].first == "WWW-Authenticate");
        CHECK(m::detail::constantTimeEquals("abc", "abc") && !m::detail::constantTimeEquals("abc", "abd") && !m::detail::constantTimeEquals("abc", "ab"));
    }

    // --- a real HTTP server on loopback, scraped while the numbers change ---------------------------------------------------------------------
    {
        m::MetricsRegistry r;
        m::StandardMetrics sm(r);
        m::HandlerConfig cfg;
        cfg.bearerToken = "tok";
        m::MetricsHandler handler(r, cfg);
        httplib::Server svr;
        svr.Get("/metrics", [&](const httplib::Request& req, httplib::Response& res) {
            const auto rep = handler.handle("GET", req.path, req.get_header_value("Authorization"));
            res.status = rep.status;
            for (const auto& hd : rep.headers) res.set_header(hd.first, hd.second);
            res.set_content(rep.body, rep.contentType);
        });
        int port = 0;
        for (int p = 18710; p < 18790 && port == 0; ++p)
            if (svr.bind_to_port("127.0.0.1", p)) port = p;
        CHECK(port != 0);
        std::thread srv([&] { svr.listen_after_bind(); });
        svr.wait_until_ready();
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            std::uint64_t n = 0;
            while (!stop) {
                m::EngineSnapshot s;
                s.decodesByDecoder = {{"adsb", ++n}, {"ais", n / 2}};
                s.signalLevelDbfs = -50.0 - static_cast<double>(n % 10);
                s.connectedClients = static_cast<int>(n % 5);
                sm.publish(s);
            }
        });
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(3, 0);
        auto denied = cli.Get("/metrics");
        CHECK(denied && denied->status == 401 && denied->get_header_value("WWW-Authenticate") == "Bearer");
        int good = 0, bad = 0;
        for (int i = 0; i < 100; ++i) {
            auto res = cli.Get("/metrics", httplib::Headers{{"Authorization", "Bearer tok"}});
            if (!res || res->status != 200) { ++bad; continue; }
            if (res->get_header_value("Content-Type") != "text/plain; version=0.0.4; charset=utf-8") { ++bad; continue; }
            const Parsed p = parse(res->body);
            if (!p.ok) { ++bad; std::printf("parse: %s\n", p.why.c_str()); continue; }
            ++good;
        }
        stop = true;
        writer.join();
        svr.stop();
        srv.join();
        std::printf("INFO scraped %d times over loopback HTTP while a writer updated the registry: %d valid, %d bad\n", good + bad, good, bad);
        CHECK(good == 100 && bad == 0);
    }

    return testSummary("test_metrics");
}
