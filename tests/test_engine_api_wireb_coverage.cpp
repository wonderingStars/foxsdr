// test_engine_api_wireb_coverage.cpp - THE ADS-B COVERAGE GRID IN THE ENGINE (wire-b; FOXAPP_OP_COVERAGE_CONFIG 0x892E, FOXAPP_OP_COVERAGE_READ 0x892F; Engine::pumpTracks over core/adsb_coverage.hpp). The tracks are
// what the FRONT END hands the engine (Engine::setFrontEndFacts); the receiver is at 51.5, -0.12 (given by the caller: the engine never reads a position from the machine).
//
//   C1  OFF by default: no fact, reading is FAILED; enabling without a position is BAD_ARGUMENT, with only one of the two is BAD_ARGUMENT; the same setting twice is NO_CHANGE
//   C2  an aircraft 64 km east at 10000 m (32808 ft) and one 111 km north at 1500 m (4921 ft) are added; the null-island position and one thousands of kilometres off are counted and refused; a track with no
//       position and a vessel are ignored; the same position again adds nothing
//   C3  the outline per bearing: 64 km in the bin of 90 degrees, 111 km in the bin of 0; the band of 30000-39999 ft holds only the first; every band reports the larger range; the heat cells are there
//   C4  the receiver position is not in any answer; clear empties; disabling drops the grid (reading FAILED again); bad keys / values refused; REMOTE DENIED
//   NOTHING is written to disk: the engine calls neither save() nor load() of the grid
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "wire1_rig.hpp"

namespace {

cascade::core::HostTrack track(const char* id, std::uint32_t kind, double lat, double lon, double altM) {
    cascade::core::HostTrack h;
    std::strncpy(h.t.id, id, sizeof(h.t.id) - 1);
    h.t.latDeg = lat;
    h.t.lonDeg = lon;
    h.t.altM = altM;
    h.t.courseDeg = 10.0;
    h.t.speedMps = 100.0;
    h.t.ageMs = 0;
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

std::string rawFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

json factJson(Rig& r, const std::string& key) {
    const json j = json::parse(rawFact(r, key), nullptr, false);
    return j.is_discarded() ? json() : j;
}

json read(Rig& r, const std::string& params) {
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_READ, params).status(), FOXAPI_OK);
    return factJson(r, "coverage.read");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_coverage\n");
    engine_api_test::hermetic("wirebcoverage");
    Access::installHooks();
    Rig r;

    std::printf("C1: off by default\n");
    CHECK(factJson(r, "coverage.state").is_null());
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_READ, "{}").status(), FOXAPI_FAILED);
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":true}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":true,\"rx_lat\":51.5}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":false}").status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":true,\"rx_lat\":51.5,\"rx_lon\":-0.12}").status(), FOXAPI_OK);
    CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":true}").status(), FOXAPI_NO_CHANGE);

    std::printf("C2: added and refused\n");
    const double nan = std::nan("");
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, 0.8, 10000.0),
             track("D4E5F6", CASCADE_TRACK_AIRCRAFT, 52.5, -0.12, 1500.0),
             track("000000", CASCADE_TRACK_AIRCRAFT, 0.0, 0.0, 3000.0),             // null island: a failed decode
             track("FAR001", CASCADE_TRACK_AIRCRAFT, 10.0, 20.0, 9000.0),           // thousands of kilometres away
             track("NOPOS1", CASCADE_TRACK_AIRCRAFT, nan, nan, 5000.0),
             track("SHIP01", CASCADE_TRACK_VESSEL, 51.6, 0.1, 0.0)});
    r.pump(15);
    json st = factJson(r, "coverage.state");
    CHECK(st.is_object());
    std::printf("  state: %s\n", st.dump().c_str());
    CHECK_EQ(st["added"].get<std::uint64_t>(), static_cast<std::uint64_t>(2));
    CHECK(st["bad_input"].get<std::uint64_t>() >= 1);
    CHECK(st["too_far"].get<std::uint64_t>() >= 1);
    CHECK(st["cells"].get<std::size_t>() >= 2);
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, 0.8, 10000.0)});
    r.pump(15);
    CHECK_EQ(factJson(r, "coverage.state")["added"].get<std::uint64_t>(), static_cast<std::uint64_t>(2));   // the same position again adds nothing

    std::printf("C3: outline, bands\n");
    {
        const json all = read(r, "{}");
        CHECK_EQ(all["bin_deg"].get<int>(), 5);
        CHECK_EQ(all["outline_km"].size(), static_cast<std::size_t>(72));
        CHECK(all["cells"].size() >= 2);
        if (all["outline_km"].size() == 72) {
            const double east = std::max(all["outline_km"][17].get<double>(), all["outline_km"][18].get<double>());   // (the great-circle bearing is 89.7 degrees: the bin below 90)
            std::printf("  east bins %.1f km, north bin %.1f km, max %.1f km\n", east, all["outline_km"][0].get<double>(), all["max_range_km"].get<double>());
            CHECK(std::fabs(east - 63.0) < 6.0);     // 0.92 degrees of longitude at 51.5 N
            CHECK(std::fabs(all["outline_km"][0].get<double>() - 111.2) < 6.0);
            CHECK(std::fabs(all["max_range_km"].get<double>() - 111.2) < 6.0);
        }
        const json hi = read(r, "{\"band\":4}");   // 30000-39999 ft: only the first aircraft
        CHECK_EQ(hi["band_label"].get<std::string>(), std::string("30000-39999 ft"));
        if (hi["outline_km"].size() == 72) {
            CHECK(std::max(hi["outline_km"][17].get<double>(), hi["outline_km"][18].get<double>()) > 50.0);
            CHECK_EQ(hi["outline_km"][0].get<double>(), 0.0);
        }
        const json lo = read(r, "{\"band\":0}");
        if (lo["outline_km"].size() == 72) { CHECK(lo["outline_km"][0].get<double>() > 100.0 && std::max(lo["outline_km"][17].get<double>(), lo["outline_km"][18].get<double>()) == 0.0); }
        // the receiver's position is in no answer
        CHECK(!all.contains("rx_lat") && !all.contains("rx_lon"));
        CHECK(rawFact(r, "coverage.state").find("rx_") == std::string::npos);
    }

    std::printf("C4: clear, disable, refusals\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"cell_deg\":5}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"rx_lat\":95,\"rx_lon\":0}").status(), FOXAPI_BAD_ARGUMENT);
        const auto unk = sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"home\":1}");
        CHECK_EQ(unk.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(unk.result.message).find("home") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_READ, "{\"band\":9}").status(), FOXAPI_BAD_ARGUMENT);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_READ, "{}", &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"clear\":true}", &remote).status(), FOXAPI_DENIED);
        feed(r, {});
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"clear\":true}").status(), FOXAPI_OK);
        CHECK_EQ(read(r, "{}")["cells"].size(), static_cast<std::size_t>(0));
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_CONFIG, "{\"enabled\":false}").status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_COVERAGE_READ, "{}").status(), FOXAPI_FAILED);
        CHECK(factJson(r, "coverage.state").is_null());
    }
    return check::finish("test_engine_api_wireb_coverage");
}
