// test_engine_api_wireb_sbs.cpp - SBS (BASESTATION) TEXT OUT OF THE AIRCRAFT TRACKS THROUGH THE ENGINE (wire-b; FOXAPP_OP_SBS_OUT_START 0x892A, FOXAPP_OP_SBS_OUT_STOP 0x892B; Engine::pumpSbs over
// net/aircraft_out.hpp SbsOut). The tracks are what the FRONT END hands the engine (Engine::setFrontEndFacts, the window's plugin poll): the test hands over its own.
//
//   B1  OFF by default: no fact, nothing listens, STOP is NO_CHANGE; a bind key is refused
//   B2  ON: an aircraft with an ICAO address and a position is sent as MSG,3 (altitude in feet, latitude, longitude) and MSG,4 (ground speed in knots, track); the lines are CRLF-ended and carry the
//       six-digit address; a track that is not an aircraft, has an id that is not six upper-case hex digits, or has no position is never sent
//   B3  only what CHANGED is sent: the same feed again sends nothing; a moved aircraft sends a position line
//   B4  no callsign (MSG,1) and no label ever leaves; STOP ends the stream (the client reads end of stream) and removes the fact; a second start is BUSY; REMOTE DENIED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "wire1_rig.hpp"

namespace {

#ifdef _WIN32
using Sk = SOCKET;
constexpr Sk kBad = INVALID_SOCKET;
void closeSk(Sk s) { ::closesocket(s); }
void startNetOnce() { static WSADATA d; static bool done = (::WSAStartup(MAKEWORD(2, 2), &d) == 0); (void)done; }
#else
using Sk = int;
constexpr Sk kBad = -1;
void closeSk(Sk s) { ::close(s); }
void startNetOnce() {}
#endif

Sk connectTcp(std::uint16_t port) {
    startNetOnce();
    const Sk s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBad) { return kBad; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { closeSk(s); return kBad; }
#ifdef _WIN32
    DWORD tmo = 400;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
    timeval tv{0, 400000};
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

// Everything that arrives within `ms` (the socket times out in 400 ms steps).
std::string drain(Sk s, int ms) {
    std::string out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    char buf[4096];
    while (std::chrono::steady_clock::now() < end) {
        const int k = ::recv(s, buf, sizeof(buf), 0);
        if (k > 0) { out.append(buf, static_cast<std::size_t>(k)); } else if (k == 0) { break; }
    }
    return out;
}

std::vector<std::vector<std::string>> lines(const std::string& text) {
    std::vector<std::vector<std::string>> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t eol = text.find("\r\n", pos);
        if (eol == std::string::npos) { break; }
        std::vector<std::string> f;
        std::size_t a = pos;
        for (std::size_t i = pos; i <= eol; ++i) {
            if (i == eol || text[i] == ',') { f.push_back(text.substr(a, i - a)); a = i + 1; }
        }
        out.push_back(std::move(f));
        pos = eol + 2;
    }
    return out;
}

cascade::core::HostTrack track(const char* id, std::uint32_t kind, double lat, double lon, double altM, double course, double speedMps) {
    cascade::core::HostTrack h;
    std::strncpy(h.t.id, id, sizeof(h.t.id) - 1);
    std::strncpy(h.t.label, "SECRETCALL", sizeof(h.t.label) - 1);
    h.t.latDeg = lat;
    h.t.lonDeg = lon;
    h.t.altM = altM;
    h.t.courseDeg = course;
    h.t.speedMps = speedMps;
    h.t.kind = kind;
    h.plugin = "adsb";
    return h;
}

void feed(Rig& r, std::vector<cascade::core::HostTrack> tracks) {
    auto f = std::make_shared<cascade::core::TrackFeed>();
    f->polled = std::move(tracks);
    cascade::engine::FrontEndFacts facts;
    facts.trackFeed = f;
    r.eng.setFrontEndFacts(std::move(facts));
}

testing::Outcome sendText(Rig& r, std::uint32_t op, const std::string& text, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(sess.raw(), &c, 1, &sr) : api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) { if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; } }
    }
    return o;
}

json sbsFact(Rig& r) {
    const json j = json::parse(r.fact("sbs.out"), nullptr, false);
    return j.is_discarded() ? json() : j;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_sbs\n");
    engine_api_test::hermetic("wirebsbs");
    Access::installHooks();
    Rig r;

    std::printf("B1: off by default\n");
    CHECK(sbsFact(r).is_null());
    CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_STOP, "").status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_START, "{\"bind\":\"0.0.0.0\"}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_START, "{\"port\":70000}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK(sbsFact(r).is_null());

    std::printf("B2: lines\n");
    const auto o = sendText(r, FOXAPP_OP_SBS_OUT_START, "{}");
    CHECK_EQ(o.status(), FOXAPI_OK);
    const std::uint16_t port = static_cast<std::uint16_t>(o.result.applied[0]);
    CHECK(port != 0);
    CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_START, "{}").status(), FOXAPI_BUSY);
    const Sk s = connectTcp(port);
    CHECK(s != kBad);
    if (s == kBad) { return check::finish("test_engine_api_wireb_sbs"); }
    r.pump(10);
    const double nan = std::nan("");
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, -0.12, 10000.0, 90.0, 200.0),
             track("BADHEX", CASCADE_TRACK_AIRCRAFT, 52.0, 1.0, 5000.0, 10.0, 100.0),            // not hex
             track("a1b2c4", CASCADE_TRACK_AIRCRAFT, 52.0, 1.0, 5000.0, 10.0, 100.0),            // lower case: not an address this format carries
             track("ABCDEF", CASCADE_TRACK_VESSEL, 50.0, 0.0, 0.0, 10.0, 5.0),                   // a ship with a hex-looking id
             track("123456", CASCADE_TRACK_AIRCRAFT, nan, nan, 5000.0, 10.0, 100.0)});           // no position
    r.pump(10);
    const std::string got = drain(s, 1500);
    const auto rows = lines(got);
    std::printf("  %zu line(s):\n%.300s\n", rows.size(), got.c_str());
    bool pos = false, vel = false;
    for (const auto& f : rows) {
        CHECK(f.size() >= 22);
        if (f.size() < 22) { continue; }
        CHECK_EQ(f[0], std::string("MSG"));
        CHECK_EQ(f[4], std::string("A1B2C3"));   // no other aircraft was sent
        if (f[1] == "3") {
            pos = true;
            CHECK_EQ(f[11], std::string("32808"));                               // 10000 m in feet
            CHECK(std::fabs(std::stod(f[14]) - 51.5) < 1e-4 && std::fabs(std::stod(f[15]) + 0.12) < 1e-4);
        } else if (f[1] == "4") {
            vel = true;
            CHECK_EQ(f[12], std::string("389"));                                 // 200 m/s in knots
            CHECK(std::fabs(std::stod(f[13]) - 90.0) < 1e-6);
        } else {
            CHECK(false);   // no MSG,1 (identification: a callsign), no squawk
        }
        CHECK(f[10].empty());   // the callsign field is empty on every line
    }
    CHECK(pos && vel);
    CHECK(got.find("SECRETCALL") == std::string::npos);

    std::printf("B3: only what changed\n");
    {
        const std::uint64_t before = sbsFact(r)["lines_sent"].get<std::uint64_t>();
        feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, -0.12, 10000.0, 90.0, 200.0)});
        r.pump(20);
        CHECK(drain(s, 700).empty());
        CHECK_EQ(sbsFact(r)["lines_sent"].get<std::uint64_t>(), before);
        feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.6, -0.12, 10000.0, 90.0, 200.0)});
        r.pump(20);
        const auto moved = lines(drain(s, 1200));
        CHECK_EQ(moved.size(), static_cast<std::size_t>(1));   // the position moved; course and speed did not
        if (moved.size() == 1 && moved[0].size() >= 16) { CHECK(moved[0][1] == "3" && std::fabs(std::stod(moved[0][14]) - 51.6) < 1e-4); }
        const json j = sbsFact(r);
        CHECK(j.is_object() && j["running"].get<bool>() && j["port"].get<int>() == static_cast<int>(port) && j["tracks_seen"].get<std::uint64_t>() == 1);
        CHECK(j["clients_accepted"].get<std::uint64_t>() >= 1);
    }

    std::printf("B4: stop, REMOTE\n");
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_STOP, "", &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_STOP, "").status(), FOXAPI_OK);
        char buf[256];
        int k = 1;
        for (int i = 0; i < 20 && k > 0; ++i) { k = ::recv(s, buf, sizeof(buf), 0); }
        CHECK(k <= 0);
        closeSk(s);
        CHECK(sbsFact(r).is_null());
        CHECK_EQ(sendText(r, FOXAPP_OP_SBS_OUT_STOP, "").status(), FOXAPI_NO_CHANGE);
    }
    return check::finish("test_engine_api_wireb_sbs");
}
