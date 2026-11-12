// web_wire_c.hpp - the web server's side of wiring lane C: the models of docs/webmodels-handoff.md joined to the server, in ONE object the server owns.
// web_server.cpp stays untouched but for the smallest bracketed calls (the object, its pre- and post-routing hooks and its route installation); everything of
// substance is here and in web_wire_c.cpp.
//
// CORS (row W7a): a pre-routing handler answers an allowed preflight with 204 and the model's headers; the post-routing hook adds the headers to every answer. A
// denied or non-CORS request is NOT stopped by CORS (that is not authentication; the sign-in gate does that). With no origin listed (the default) the policy is
// unconfigured: no header is ever added, so the server behaves exactly as before. CREDENTIALS are never allowed: the session cookie is SameSite=Strict, so a
// cross-origin page could not send it anyway, and changing that is an owner decision that needs HTTPS (docs/webmodels-handoff.md 7a).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "net/web_cors.hpp"

namespace httplib {
struct Request;
struct Response;
}  // namespace httplib

namespace cascade::net {

class WebWireC {
public:
    // CORS: `origins` are the allowed origins ("https://example.com", "http://192.168.1.20:8080"); an empty list switches CORS off. False with `error` set when an origin is
    // not acceptable (nothing changes). Callable while the server runs.
    bool setCors(const std::vector<std::string>& origins, std::string& error);

    // True when the request was answered here (an allowed preflight: 204 + headers). Called before routing.
    bool preRouting(const httplib::Request& req, httplib::Response& res);
    // Adds the model's headers (Vary, Access-Control-Allow-Origin ...) to an answer. Called after routing, for every answer.
    void addHeaders(const httplib::Request& req, httplib::Response& res);

private:
    mutable std::mutex mu_;
    CorsPolicy cors_;
};

}  // namespace cascade::net
