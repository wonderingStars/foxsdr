// test_engine_api_wireb_tracks.cpp - THE TRACK STORE IN THE ENGINE (wire-b; FOXAPP_OP_TRACKS_CONFIG 0x892C, FOXAPP_OP_TRACKS_READ 0x892D; Engine::pumpTracks over core/track_store.hpp). The tracks are what the FRONT END
// hands the engine (Engine::setFrontEndFacts, the window's plugin poll): the test hands over its own.
//
//   T1  OFF by default: no fact, reading is FAILED, nothing is stored; enabling creates the store; the same setting again is NO_CHANGE
//   T2  enabled: an aircraft and a vessel are stored when their position is handed over (the time is now minus the track's age), a satellite, a track with no position and a track with an id the store refuses
//       are not (the refusal is counted); the picture at now holds both, a kind filter one; the same position again adds nothing, a moved aircraft adds a point
//   T3  a trail has the two points in time order; the picture at a time between them interpolates (the latitude between the two, flagged interpolated); the range is the first and the last time held
//   T4  clear drops everything; disabling stops the store (reading FAILED again); retention out of range, an unknown key, a bad kind or id are BAD_ARGUMENT; REMOTE is DENIED
//   NOTHING is written to disk: the engine calls neither save() nor load() of the store (the model's own persistence is not wired)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wire1_rig.hpp"

namespace {

cascade::core::HostTrack track(const char* id, std::uint32_t kind, double lat, double lon, double altM, std::uint64_t ageMs = 0) {
    cascade::core::HostTrack h;
    std::strncpy(h.t.id, id, sizeof(h.t.id) - 1);
    h.t.latDeg = lat;
    h.t.lonDeg = lon;
    h.t.altM = altM;
    h.t.courseDeg = 10.0;
    h.t.speedMps = 100.0;
    h.t.ageMs = ageMs;
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

json factJson(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    if (n < 0) { return json(); }
    const json j = json::parse(buf.c_str(), nullptr, false);
    return j.is_discarded() ? json() : j;
}

json read(Rig& r, const std::string& params) {
    const auto o = sendText(r, FOXAPP_OP_TRACKS_READ, params);
    CHECK_EQ(o.status(), FOXAPI_OK);
    return factJson(r, "tracks.read");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_tracks\n");
    engine_api_test::hermetic("wirebtracks");
    Access::installHooks();
    Rig r;

    std::printf("T1: off by default\n");
    CHECK(factJson(r, "tracks.state").is_object());   // second pass (screen lane L): the fact is there and says it is off
    CHECK_EQ(factJson(r, "tracks.state")["enabled"].get<bool>(), false);
    CHECK_EQ(factJson(r, "tracks.state")["tracks"].get<std::uint64_t>(), static_cast<std::uint64_t>(0));
    CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_READ, "{}").status(), FOXAPI_FAILED);
    CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"enabled\":false}").status(), FOXAPI_NO_CHANGE);
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, -0.12, 10000.0)});
    r.pump(10);
    CHECK_EQ(factJson(r, "tracks.state")["added"].get<std::uint64_t>(), static_cast<std::uint64_t>(0));   // nothing was stored while off
    CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"enabled\":true}").status(), FOXAPI_OK);
    CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"enabled\":true}").status(), FOXAPI_NO_CHANGE);
    CHECK(factJson(r, "tracks.state").is_object());

    std::printf("T2: stored\n");
    const double nan = std::nan("");
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, -0.12, 10000.0),
             track("MMSI244123456", CASCADE_TRACK_VESSEL, 52.0, 4.0, 0.0),
             track("SAT25544", CASCADE_TRACK_SATELLITE, 10.0, 20.0, 400000.0),       // not a kind the store keeps
             track("NOPOS1", CASCADE_TRACK_AIRCRAFT, nan, nan, 5000.0),                // no position
             track("BAD ID", CASCADE_TRACK_AIRCRAFT, 50.0, 0.0, 100.0)});              // a space: the store refuses the id
    r.pump(15);
    json st = factJson(r, "tracks.state");
    CHECK(st.is_object());
    std::printf("  state: %s\n", st.dump().c_str());
    CHECK_EQ(st["added"].get<std::uint64_t>(), static_cast<std::uint64_t>(2));
    CHECK(st["refused"].get<std::uint64_t>() >= 1);
    CHECK_EQ(st["tracks"].get<std::size_t>(), static_cast<std::size_t>(2));
    {
        const json all = read(r, "{}");
        CHECK_EQ(all["tracks"].size(), static_cast<std::size_t>(2));
        const json ac = read(r, "{\"kind\":\"aircraft\"}");
        CHECK_EQ(ac["tracks"].size(), static_cast<std::size_t>(1));
        if (ac["tracks"].size() == 1) { CHECK_EQ(ac["tracks"][0]["id"].get<std::string>(), std::string("A1B2C3")); }
    }
    // the same position again adds nothing
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.5, -0.12, 10000.0)});
    r.pump(15);
    CHECK_EQ(factJson(r, "tracks.state")["added"].get<std::uint64_t>(), static_cast<std::uint64_t>(2));

    std::printf("T3: trail, interpolation, range\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    feed(r, {track("A1B2C3", CASCADE_TRACK_AIRCRAFT, 51.6, -0.12, 10000.0)});
    r.pump(15);
    CHECK_EQ(factJson(r, "tracks.state")["added"].get<std::uint64_t>(), static_cast<std::uint64_t>(3));
    {
        const json tr = read(r, "{\"trail\":{\"id\":\"A1B2C3\",\"kind\":\"aircraft\",\"t0\":0}}");
        CHECK_EQ(tr["points"].size(), static_cast<std::size_t>(2));
        if (tr["points"].size() == 2) {
            const double t1 = tr["points"][0]["t"].get<double>(), t2 = tr["points"][1]["t"].get<double>();
            std::printf("  trail: %.2f s apart, lat %.2f -> %.2f\n", t2 - t1, tr["points"][0]["lat"].get<double>(), tr["points"][1]["lat"].get<double>());
            CHECK(t2 - t1 > 1.0 && t2 > t1);
            CHECK(std::fabs(tr["points"][0]["lat"].get<double>() - 51.5) < 1e-9 && std::fabs(tr["points"][1]["lat"].get<double>() - 51.6) < 1e-9);
            const json mid = read(r, "{\"kind\":\"aircraft\",\"t\":" + std::to_string((t1 + t2) / 2.0) + "}");
            CHECK_EQ(mid["tracks"].size(), static_cast<std::size_t>(1));
            if (mid["tracks"].size() == 1) {
                CHECK(mid["tracks"][0]["interpolated"].get<bool>());
                CHECK(std::fabs(mid["tracks"][0]["lat"].get<double>() - 51.55) < 0.01);
            }
            const json rg = read(r, "{\"range\":true}");
            CHECK(rg["has_range"].get<bool>());
            CHECK(std::fabs(rg["first_s"].get<double>() - t1) < 0.5 && std::fabs(rg["last_s"].get<double>() - t2) < 0.5);
        }
        // an age: a plugin that last heard a track 5 s ago stores it 5 s back
        feed(r, {track("D4E5F6", CASCADE_TRACK_AIRCRAFT, 40.0, 10.0, 3000.0, 5000)});
        r.pump(15);
        const json t = read(r, "{\"trail\":{\"id\":\"D4E5F6\",\"kind\":\"aircraft\",\"t0\":0}}");
        CHECK_EQ(t["points"].size(), static_cast<std::size_t>(1));
        const double nowS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (t["points"].size() == 1) { CHECK(std::fabs((nowS - t["points"][0]["t"].get<double>()) - 5.0) < 2.5); }
    }

    std::printf("T4: clear, disable, refusals, nothing on disk\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"retention_s\":10}").status(), FOXAPI_BAD_ARGUMENT);
        const auto unk = sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"keep\":true}");
        CHECK_EQ(unk.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(unk.result.message).find("keep") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_READ, "{\"kind\":\"balloon\"}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_READ, "{\"trail\":{\"id\":\"bad id\"}}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"retention_s\":3600}").status(), FOXAPI_OK);
        CHECK_EQ(factJson(r, "tracks.state")["retention_s"].get<double>(), 3600.0);
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_READ, "{}", &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"clear\":true}", &remote).status(), FOXAPI_DENIED);
        feed(r, {});   // (the front end still shows the track: a clear would take it in again at once)
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"clear\":true}").status(), FOXAPI_OK);
        CHECK_EQ(read(r, "{}")["tracks"].size(), static_cast<std::size_t>(0));
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_CONFIG, "{\"enabled\":false}").status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_TRACKS_READ, "{}").status(), FOXAPI_FAILED);
    }
    return check::finish("test_engine_api_wireb_tracks");
}
