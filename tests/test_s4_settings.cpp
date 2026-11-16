// Tests for the S4 keys of the wirec settings registry (engine/wire_c.cpp): their defaults (every output OFF, every address this computer), their validation, and the rule that the
// MQTT password and the webhook address live in the protected store, never in wirec.json, never in a published fact. Files go in a scratch folder of this test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c.hpp"

#include "test_check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace wc = cascade::engine::wirec;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path scratch() {
    const fs::path d = fs::temp_directory_path() / ("foxsdr-s4-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(d);
    return d;
}

void testDefaultsAndValidation() {
    const fs::path dir = scratch();
    wc::Settings s(dir / "wirec.json");
    CHECK(s.get("out.mqtt.enabled") == "0" && s.get("out.webhook.enabled") == "0" && s.get("alerts.enabled") == "0");
    CHECK(s.get("out.mqtt.host") == "127.0.0.1" && s.get("out.mqtt.port") == "1883" && s.get("out.mqtt.topic") == "foxsdr/decodes");
    CHECK(s.get("out.webhook.url").empty() && s.get("out.mqtt.password").empty() && s.get("alerts.gap_s") == "30" && s.get("alerts.notify") == "1");
    std::string why;
    CHECK(s.set("out.mqtt.port", "0", why) == wc::SetResult::OutOfRange);
    CHECK(s.set("out.mqtt.port", "65536", why) == wc::SetResult::OutOfRange);
    CHECK(s.set("out.mqtt.port", "65535", why) == wc::SetResult::Changed);
    CHECK(s.set("out.mqtt.host", "bad host", why) == wc::SetResult::BadValue && !why.empty());
    CHECK(s.set("out.mqtt.topic", "a/#", why) == wc::SetResult::BadValue);
    CHECK(s.set("out.webhook.url", "ftp://x", why) == wc::SetResult::BadValue);
    CHECK(s.set("out.webhook.send", "everything", why) == wc::SetResult::OutOfRange);
    CHECK(s.set("alerts.gap_s", "3601", why) == wc::SetResult::OutOfRange && s.set("alerts.gap_s", "-1", why) == wc::SetResult::OutOfRange);
    CHECK(s.set("alerts.keywords", std::string(121, 'k'), why) == wc::SetResult::BadValue);
    CHECK(s.set("alerts.keywords", "mayday\nfire", why) == wc::SetResult::Changed && s.get("alerts.keywords") == "mayday\nfire");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void testSecretsNeverInTheFileOrTheFacts() {
    const fs::path dir = scratch();
    const std::string secretPass = "p4ssw0rd-hunter2-unique";
    const std::string secretUrl = "https://example.org/hook/token-abcdef-unique";
    {
        wc::Settings s(dir / "wirec.json");
        std::string why;
        CHECK(s.set("out.mqtt.password", secretPass, why) == wc::SetResult::Changed);
        CHECK(s.set("out.webhook.url", secretUrl, why) == wc::SetResult::Changed);
        CHECK(s.set("out.mqtt.user", "someone", why) == wc::SetResult::Changed);
        CHECK(s.get("out.mqtt.password") == secretPass && s.get("out.webhook.url") == secretUrl);   // the engine reads them
        const std::string file = slurp(dir / "wirec.json");
        CHECK(!file.empty() && file.find("unique") == std::string::npos);     // neither value is in the settings file
        CHECK(file.find("someone") != std::string::npos);                     // an ordinary key is
        std::map<std::string, std::string> facts;
        s.each([&](const std::string& k, const std::string& v) { facts[k] = v; });
        CHECK(facts["out.mqtt.password"] == "set" && facts["out.webhook.url"] == "set");   // a fact says only that there is one
        for (const auto& [k, v] : facts) { CHECK(v.find("unique") == std::string::npos); }
    }
    {
        wc::Settings again(dir / "wirec.json");   // a new start: the values come back from the protected store
        CHECK(again.get("out.mqtt.password") == secretPass && again.get("out.webhook.url") == secretUrl && again.get("out.mqtt.user") == "someone");
        std::string why;
        CHECK(again.set("out.mqtt.password", "", why) == wc::SetResult::Changed);   // cleared
    }
    {
        wc::Settings third(dir / "wirec.json");
        CHECK(third.get("out.mqtt.password").empty() && third.get("out.webhook.url") == secretUrl);
        std::map<std::string, std::string> facts;
        third.each([&](const std::string& k, const std::string& v) { facts[k] = v; });
        CHECK(facts["out.mqtt.password"].empty() && facts["out.webhook.url"] == "set");
    }
    // a value an older build left in the file in plain text is moved to the store on the first read
    {
        const fs::path d2 = scratch();
        {
            std::ofstream f(d2 / "wirec.json", std::ios::binary);
            f << "{\"out.mqtt.password\":\"legacy-plain-unique\",\"out.mqtt.user\":\"u\"}\n";
        }
        wc::Settings s(d2 / "wirec.json");
        CHECK(s.get("out.mqtt.password") == "legacy-plain-unique");
        CHECK(slurp(d2 / "wirec.json").find("legacy-plain-unique") == std::string::npos);
        std::error_code ec;
        fs::remove_all(d2, ec);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    testDefaultsAndValidation();
    testSecretsNeverInTheFileOrTheFacts();
    return testSummary("test_s4_settings");
}
