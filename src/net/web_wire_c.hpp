// web_wire_c.hpp - the web server's side of wiring lane C: the models of docs/webmodels-handoff.md joined to the server, in ONE object the server owns.
// web_server.cpp stays untouched but for the smallest bracketed calls (the object, its pre- and post-routing hooks and its route installation); everything of
// substance is here and in web_wire_c.cpp.
//
// CORS (row W7a): a pre-routing handler answers an allowed preflight with 204 and the model's headers; the post-routing hook adds the headers to every answer. A
// denied or non-CORS request is NOT stopped by CORS (that is not authentication; the sign-in gate does that). With no origin listed (the default) the policy is
// unconfigured: no header is ever added, so the server behaves exactly as before. CREDENTIALS are never allowed: the session cookie is SameSite=Strict, so a
// cross-origin page could not send it anyway, and changing that is an owner decision that needs HTTPS (docs/webmodels-handoff.md 7a).
//
// MY BOOKMARKS (row W2): a viewer's own bookmarks, kept in memory per sign-in (the session cookie's value is the key; it is never logged, stored or sent to
// anyone). Routes, all behind the server's own sign-in rule: GET/POST /api/mybookmarks, PUT/DELETE /api/mybookmarks/<id>, GET /api/mybookmarks/export,
// POST /api/mybookmarks/import?replace=0|1. They never touch the receiver (a listener may use them). The set is forgotten at sign-out and when the sign-in has
// ended. Where the server needs no sign-in (loopback, no password) there are no sessions, so every browser shares ONE set.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "net/metrics.hpp"
#include "net/web_bookmarks.hpp"
#include "net/web_cors.hpp"

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace cascade::net {

// What the server tells this object about a request (so none of the server's internals leak here).
struct WebWireCHooks {
    std::function<bool(const httplib::Request&)> signedIn;                 // the server's own sign-in rule: true when the request may use the page
    std::function<std::string(const httplib::Request&)> viewerToken;       // the session's token ("" when the server has no sessions)
    std::function<bool(const std::string&)> tokenStillValid;               // the sign-in behind a token has not ended
    std::function<void(httplib::Response&, const std::string&, int)> deny; // the server's one refusal shape
};

class WebWireC {
public:
    // CORS: `origins` are the allowed origins ("https://example.com", "http://192.168.1.20:8080"); an empty list switches CORS off. False with `error` set when an origin is
    // not acceptable (nothing changes). Callable while the server runs.
    bool setCors(const std::vector<std::string>& origins, std::string& error);

    // True when the request was answered here (an allowed preflight: 204 + headers). Called before routing.
    bool preRouting(const httplib::Request& req, httplib::Response& res);
    // Adds the model's headers (Vary, Access-Control-Allow-Origin ...) to an answer. Called after routing, for every answer.
    void addHeaders(const httplib::Request& req, httplib::Response& res);

    // Registers this lane's routes on the server (called once, from installRoutes).
    void installRoutes(httplib::Server& svr, WebWireCHooks hooks);

    // The viewer's sign-in ended (sign-out): drop what is kept for them.
    void forgetViewer(const std::string& token);

    // METRICS PAGE (row L3; net/metrics.hpp): GET /metrics in the Prometheus text format, for a SIGNED-IN browser only (the server's own sign-in rule; no token of any
    // kind is kept: a scraper that cannot use the cookie is an owner decision). Off (404, as if it did not exist) until switched on.
    void setMetricsEnabled(bool on) { metricsOn_.store(on, std::memory_order_relaxed); }
    bool metricsEnabled() const { return metricsOn_.load(std::memory_order_relaxed); }
    void publishMetrics(const metrics::EngineSnapshot& s);

private:
    void installBookmarkRoutes(httplib::Server& svr);
    void installMetricsRoute(httplib::Server& svr);
    std::string viewerOf(const httplib::Request& req) const;
    void pruneViewers();

    WebWireCHooks hooks_;
    mutable std::mutex mu_;
    CorsPolicy cors_;
    WebBookmarkBook bookmarks_;
    std::atomic<bool> metricsOn_{false};
    metrics::MetricsRegistry metricsRegistry_;
    metrics::StandardMetrics standardMetrics_{metricsRegistry_};
};

}  // namespace cascade::net
