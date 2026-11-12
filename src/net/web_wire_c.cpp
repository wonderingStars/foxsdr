// web_wire_c.cpp - see web_wire_c.hpp.
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

namespace {

CorsRequest corsRequestOf(const httplib::Request& req) {
    CorsRequest r;
    r.method = req.method;
    const auto range = req.headers.equal_range("Origin");
    for (auto it = range.first; it != range.second; ++it) { r.originValues.push_back(it->second); }   // EVERY Origin header (the model denies two)
    r.hasRequestMethod = req.has_header("Access-Control-Request-Method");
    if (r.hasRequestMethod) { r.requestMethod = req.get_header_value("Access-Control-Request-Method"); }
    r.hasRequestHeaders = req.has_header("Access-Control-Request-Headers");
    if (r.hasRequestHeaders) { r.requestHeaders = req.get_header_value("Access-Control-Request-Headers"); }
    return r;
}

}  // namespace

bool WebWireC::setCors(const std::vector<std::string>& origins, std::string& error) {
    error.clear();
    CorsPolicy fresh;   // unconfigured = off
    if (!origins.empty()) {
        CorsConfig cfg;
        cfg.origins = origins;
        cfg.allowCredentials = false;
        if (!fresh.configure(cfg, error)) { return false; }
    }
    std::lock_guard<std::mutex> lk(mu_);
    cors_ = std::move(fresh);
    return true;
}

bool WebWireC::preRouting(const httplib::Request& req, httplib::Response& res) {
    CorsDecision d;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cors_.configured()) { return false; }
        d = cors_.evaluate(corsRequestOf(req));
    }
    if (d.verdict != CorsVerdict::Preflight) { return false; }
    for (const auto& h : d.headers) { res.set_header(h.first, h.second); }
    res.status = 204;
    return true;
}

void WebWireC::addHeaders(const httplib::Request& req, httplib::Response& res) {
    CorsDecision d;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cors_.configured()) { return; }
        d = cors_.evaluate(corsRequestOf(req));
    }
    if (d.verdict == CorsVerdict::NotCors || d.verdict == CorsVerdict::Denied || d.verdict == CorsVerdict::Allowed) {
        for (const auto& h : d.headers) { res.set_header(h.first, h.second); }
    }
}

}  // namespace cascade::net
