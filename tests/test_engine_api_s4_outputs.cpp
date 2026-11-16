// test_engine_api_s4_outputs.cpp - screen lane S4's outputs and alerts through the REAL engine: the wirec settings op, the engine's pass reading the host's event ring, the facts it publishes.
// A stand-in web server on 127.0.0.1 and a recording notification stand-in: nothing leaves this computer and nothing is shown or heard on the desktop.
//
//   S1  everything off: events are not read, nothing is counted
//   S2  keyword alerts: a line with a keyword raises one alert (a private plugin's line raises none), the sink is handed it, the fact lists it
//   S3  the web address: the lines posted AFTER the switch go out (no backlog), the address is a secret (never a fact, never in wirec.json), a bad one is refused
//   S4  MQTT: a bad host or topic is refused by the settings op and changes nothing
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "http_post_stub.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class F>
bool until(Rig& r, F f, int tries = 80) {
    for (int i = 0; i < tries; ++i) {
        r.pass(2);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return f();
}

void testOff(const fs::path& dir) {
    Rig rig(dir);
    rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "mayday mayday", 148.0e6, 0);
    rig.pass(6);
    CHECK(rig.fact("out.seen") == "0" && rig.fact("alerts.raised") == "0" && rig.fact("out.webhook.queued") == "0");
    CHECK(rig.fact("wirec.out.mqtt.enabled") == "0" && rig.fact("wirec.out.webhook.enabled") == "0" && rig.fact("wirec.alerts.enabled") == "0");
    CHECK(rig.fact("wirec.out.mqtt.host") == "127.0.0.1");
}

void testAlerts(const fs::path& dir) {
    Rig rig(dir);
    auto rec = std::make_shared<cascade::core::alerts::RecordingSink>();
    Access::wirec(rig.eng).outputs.setNotifier(rec);
    CHECK(rig.status(setting("alerts.keywords", "mayday\nfire")) == FOXAPI_OK);
    CHECK(rig.status(setting("alerts.enabled", "1")) == FOXAPI_OK);
    rig.pass(4);
    rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "this is a MAYDAY call", 148.0e6, 1000000);
    rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "weather report", 148.0e6, 1001000);
    rig.eng.pluginApiShared()->hostPostEvent("secret-decoder", true, "text", "fire in a private plugin", 148.0e6, 1002000);
    CHECK(until(rig, [&] { return rig.fact("alerts.raised") == "1"; }));
    CHECK(until(rig, [&] { return rig.fact("alerts.notified") == "1"; }));
    CHECK(rec->items().size() == 1);
    CHECK(rig.fact("alerts.recent").find("MAYDAY") != std::string::npos && rig.fact("alerts.recent").find("private plugin") == std::string::npos);
    CHECK(rig.fact("out.held_private") == "1" && rig.fact("alerts.problem").empty());
    // a keyword list the engine cannot keep is refused and nothing changes
    CHECK(rig.status(setting("alerts.keywords", std::string(121, 'k'))) != FOXAPI_OK);
    CHECK(rig.fact("wirec.alerts.keywords") == "mayday\nfire");
    // switched off: the next line raises nothing
    CHECK(rig.status(setting("alerts.enabled", "0")) == FOXAPI_OK);
    rig.pass(4);
    rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "mayday again", 148.0e6, 9000000);
    rig.pass(6);
    CHECK(rig.fact("alerts.raised") == "1");
}

void testWebhook(const fs::path& dir) {
    StubServer stub("/hook-secret-path");
    CHECK(stub.start(StubServer::Mode::Accept200));
    const std::string url = stub.url();
    {
        Rig rig(dir);
        // a line posted BEFORE the switch is never sent
        rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "before the switch", 148.0e6, 0);
        CHECK(rig.status(setting("out.webhook.url", "ftp://nowhere")) != FOXAPI_OK);
        CHECK(rig.status(setting("out.webhook.url", url)) == FOXAPI_OK);
        CHECK(rig.status(setting("out.webhook.enabled", "1")) == FOXAPI_OK);
        rig.pass(4);
        rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "after the switch", 148.0e6, 0);
        rig.eng.pluginApiShared()->hostPostEvent("secret-decoder", true, "text", "private line", 148.0e6, 0);
        CHECK(until(rig, [&] { return rig.fact("out.webhook.ok") == "1"; }, 120));
        const auto bodies = stub.bodies();
        CHECK(bodies.size() == 1 && bodies[0].find("after the switch") != std::string::npos);
        for (const std::string& b : bodies) { CHECK(b.find("before the switch") == std::string::npos && b.find("private line") == std::string::npos); }
        // the address is a secret: a fact says only that there is one, the settings file never holds it
        CHECK(rig.fact("wirec.out.webhook.url") == "set");
        CHECK(rig.fact("out.webhook.scope") == "this computer only");
        const std::string file = slurp(dir / "wirec.json");
        CHECK(!file.empty() && file.find("hook-secret-path") == std::string::npos);
        for (const char* f : {"out.webhook.problem", "out.webhook.scope", "out.mqtt.problem"}) { CHECK(rig.fact(f).find("hook-secret-path") == std::string::npos); }
    }
    {
        Rig again(dir);   // a restart: the address is back from the protected store and the switch was saved
        CHECK(again.fact("wirec.out.webhook.enabled") == "1" && again.fact("wirec.out.webhook.url") == "set");
    }
}

void testMqttRefusals(const fs::path& dir) {
    Rig rig(dir);
    CHECK(rig.status(setting("out.mqtt.host", "bad host")) != FOXAPI_OK);
    CHECK(rig.status(setting("out.mqtt.topic", "a/#")) != FOXAPI_OK);
    CHECK(rig.status(setting("out.mqtt.port", "0")) != FOXAPI_OK);
    CHECK(rig.fact("wirec.out.mqtt.host") == "127.0.0.1" && rig.fact("wirec.out.mqtt.topic") == "foxsdr/decodes" && rig.fact("wirec.out.mqtt.port") == "1883");
    CHECK(rig.status(setting("out.mqtt.password", "hunter2-unique")) == FOXAPI_OK);
    CHECK(rig.fact("wirec.out.mqtt.password") == "set");
    CHECK(slurp(dir / "wirec.json").find("hunter2-unique") == std::string::npos);
    // InfluxDB: the same rules; the token is a secret; a bad bucket, host or port is refused
    CHECK(rig.status(setting("out.influx.bucket", "bad bucket")) != FOXAPI_OK);
    CHECK(rig.status(setting("out.influx.host", "a/b")) != FOXAPI_OK);
    CHECK(rig.status(setting("out.influx.every_s", "0")) != FOXAPI_OK);
    CHECK(rig.fact("wirec.out.influx.bucket") == "foxsdr" && rig.fact("wirec.out.influx.enabled") == "0" && rig.fact("wirec.out.influx.mode") == "http");
    CHECK(rig.status(setting("out.influx.token", "influx-token-unique")) == FOXAPI_OK);
    CHECK(rig.fact("wirec.out.influx.token") == "set" && slurp(dir / "wirec.json").find("influx-token-unique") == std::string::npos);
    CHECK(rig.fact("out.influx.scope").empty());   // off: nothing to say
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4_outputs");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("s4out");
    int n = 0;
    for (auto fn : {testOff, testAlerts, testWebhook, testMqttRefusals}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_s4_outputs");
}
