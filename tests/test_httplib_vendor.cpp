// Proves the vendored cpp-httplib actually works in THIS build, rather than
// merely that a header was downloaded into third_party/.
//
// WHY THIS TEST EXISTS AS A TEST RATHER THAN AS A NOTE IN THE PIN TABLE. A
// vendored header can be present, correctly hashed, and still be useless here:
// it might not compile under /W4 /permissive-, it might need an import library
// nobody linked, or its socket layer might not come up on this platform at all.
// None of that is visible from the file on disk. So this binds a real server to
// loopback on an ephemeral port and makes a real request through it, which is
// the smallest thing that can fail for any of those reasons.
//
// LOOPBACK AND AN EPHEMERAL PORT, deliberately: binding 127.0.0.1 rather than
// 0.0.0.0 keeps the listener off every other interface (and away from the
// Windows Firewall prompt that a wildcard bind provokes), and letting the OS
// choose the port means two of these running at once — or a developer with
// something already on a fixed port — cannot collide.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <string>
#include <thread>

#include <httplib.h>

#include "test_check.hpp"

namespace {

void testServeOverLoopback() {
    httplib::Server svr;

    svr.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("hello from foxsdr", "text/plain");
    });
    svr.Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(req.body, "text/plain");
    });

    const int port = svr.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    if (port <= 0) {
        // Without a listening socket nothing below can mean anything, and
        // every later call would block or crash rather than report. Bail out
        // instead of producing a cascade of misleading failures.
        return;
    }

    std::thread serverThread([&svr]() { svr.listen_after_bind(); });
    svr.wait_until_ready();
    CHECK(svr.is_running());

    {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(5, 0);

        auto res = cli.Get("/hello");
        CHECK(static_cast<bool>(res));
        if (res) {
            CHECK(res->status == 200);
            CHECK(res->body == "hello from foxsdr");
        }

        // A body must survive the round trip intact, including bytes that a
        // naive framing bug would truncate or mangle.
        const std::string payload = std::string("binary\0bytes\r\nand newlines", 26);
        auto posted = cli.Post("/echo", payload, "application/octet-stream");
        CHECK(static_cast<bool>(posted));
        if (posted) {
            CHECK(posted->status == 200);
            CHECK(posted->body == payload);
        }

        // An unregistered route must 404 rather than serve anything. This is
        // the property the real server's authentication will sit on top of:
        // routes that were never declared do not exist.
        auto missing = cli.Get("/no-such-route");
        CHECK(static_cast<bool>(missing));
        if (missing) {
            CHECK(missing->status == 404);
        }
    }

    svr.stop();
    serverThread.join();
    CHECK(!svr.is_running());
}

}  // namespace

int main() {
    testServeOverLoopback();
    return testSummary("test_httplib_vendor");
}
