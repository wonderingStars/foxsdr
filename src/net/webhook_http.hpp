// webhook_http.hpp - the plain-HTTP Transport for net/webhook.hpp, on the project's vendored cpp-httplib. HTTP ONLY: supportsHttps() is
// false, and an https URL never reaches post() (the sender refuses it first); if one did, post() refuses it itself.
//
// Kept in its own header so that webhook.hpp (and its tests) need no HTTP library. On non-Windows builds httplib.h must be included
// with CPPHTTPLIB_OPENSSL_SUPPORT like every other translation unit of the program (a layout mismatch is a documented SEGV, see
// net/web_server.cpp); this header does that. It never enables TLS use: no https URL is ever connected.
//
// Redirects are not followed (cpp-httplib does not follow them unless set_follow_location(true) is called, and it is not). One
// connection per attempt, no keep-alive. Time-outs are the two arguments; an exception escaping the library is caught and reported.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <exception>
#include <string>

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#endif
#include <httplib.h>

#include "net/webhook.hpp"

namespace cascade::net::webhook {

class HttpTransport : public Transport {
public:
    bool supportsHttps() const override { return false; }

    PostResult post(const ParsedUrl& url, const std::string& body, const std::string& contentType, int connectTimeoutMs, int ioTimeoutMs) override {
        PostResult r;
        if (url.scheme != "http") {
            r.kind = PostResult::Kind::NetworkError;
            r.error = "only http is supported";
            return r;
        }
        try {
            const auto t0 = std::chrono::steady_clock::now();
            httplib::Client cli(url.host, url.port);
            cli.set_connection_timeout(connectTimeoutMs / 1000, (connectTimeoutMs % 1000) * 1000);
            cli.set_read_timeout(ioTimeoutMs / 1000, (ioTimeoutMs % 1000) * 1000);
            cli.set_write_timeout(ioTimeoutMs / 1000, (ioTimeoutMs % 1000) * 1000);
            cli.set_keep_alive(false);
            httplib::Headers headers{{"User-Agent", "FoxSDR-webhook/1"}};
            auto res = cli.Post(url.path, headers, body, contentType);
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (!res) {
                const httplib::Error e = res.error();
                r.error = httplib::to_string(e);
                const bool timedOut = (e == httplib::Error::Read || e == httplib::Error::Write) ? ms >= ioTimeoutMs * 9 / 10
                                                                                                    : (e == httplib::Error::Connection && ms >= connectTimeoutMs * 9 / 10);
                r.kind = timedOut ? PostResult::Kind::Timeout : PostResult::Kind::NetworkError;
                return r;
            }
            r.status = res->status;
            r.kind = (res->status >= 200 && res->status < 300) ? PostResult::Kind::Ok : PostResult::Kind::HttpError;
            return r;
        } catch (const std::exception& e) {
            r.kind = PostResult::Kind::NetworkError;
            r.error = e.what();
        } catch (...) {
            r.kind = PostResult::Kind::NetworkError;
            r.error = "unknown error";
        }
        return r;
    }
};

}  // namespace cascade::net::webhook
