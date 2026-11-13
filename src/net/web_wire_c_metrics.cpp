// web_wire_c_metrics.cpp - the metrics page (wiring lane C row L3): net/metrics.hpp on the web server. See web_wire_c.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_wire_c.hpp"

// Must match every other TU in the program that includes httplib.h on non-Windows (see web_server.cpp for the ASan trace this was found from).
#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

namespace cascade::net {

void WebWireC::publishMetrics(const metrics::EngineSnapshot& s) {
    if (!metricsOn_.load(std::memory_order_relaxed)) { return; }
    standardMetrics_.publish(s);
}

void WebWireC::installMetricsRoute(httplib::Server& svr) {
    svr.Get("/metrics", [this](const httplib::Request& req, httplib::Response& res) {
        // OFF is "not there": a page that is switched off answers exactly as an unknown path does, so a scan learns nothing
        if (!metricsOn_.load(std::memory_order_relaxed)) {
            res.status = 404;
            res.set_content("not found\n", "text/plain; charset=utf-8");
            return;
        }
        if (!(hooks_.signedIn && hooks_.signedIn(req))) {
            if (hooks_.deny) { hooks_.deny(res, "sign in first", 401); } else { res.status = 401; }
            return;
        }
        const metrics::MetricsHandler handler(metricsRegistry_);
        const metrics::HttpReply r = handler.handle("GET", "/metrics");
        res.status = r.status;
        for (const auto& h : r.headers) { res.set_header(h.first, h.second); }
        res.set_content(r.body, r.contentType);
    });
}

}  // namespace cascade::net
