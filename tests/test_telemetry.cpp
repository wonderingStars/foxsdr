// Tests for core/telemetry.hpp.
//
// The point of most of these is NEGATIVE: proving that things which must
// never be transmitted are not in the payload. A privacy promise in a README
// is worth exactly as much as the test that holds the payload to it, so the
// serial-number stripping and the field inventory are asserted explicitly
// rather than left to inspection.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

void testDeviceSerialIsStripped() {
    // THE ONE THAT MATTERS. This is the real argument string for the B200 on
    // the development machine, and it contains the serial twice - once on its
    // own and once embedded in the label.
    const std::string real =
        "driver=uhd, label=B200 EDR04ZDB2, name=, product=B200, "
        "serial=EDR04ZDB2, type=b200";
    const std::string got = sanitiseDevice(real);
    CHECK(got.find("EDR04ZDB2") == std::string::npos);
    CHECK(got.find("edr04zdb2") == std::string::npos);
    // ...while still saying which radio it is, which is the whole point.
    CHECK(got.find("uhd") != std::string::npos);
    CHECK(got.find("b200") != std::string::npos);

    // A driver inventing its own keys must not leak them: the allow list is
    // positive, so anything unrecognised is dropped rather than passed on.
    const std::string nosy =
        "driver=rtlsdr, serial=00000001, addr=192.168.1.40, "
        "uri=usb://1-2, hostname=steve-pc, product=RTL2838";
    const std::string s2 = sanitiseDevice(nosy);
    CHECK(s2.find("192.168.1.40") == std::string::npos);
    CHECK(s2.find("steve-pc") == std::string::npos);
    CHECK(s2.find("usb://") == std::string::npos);
    CHECK(s2.find("00000001") == std::string::npos);
    CHECK(s2.find("rtlsdr") != std::string::npos);

    CHECK(sanitiseDevice("").empty());
    CHECK(sanitiseDevice("garbage with no equals").empty());
}

void testInstallIdIsRandomAndValidated() {
    const std::string a = newInstallId();
    const std::string b = newInstallId();
    CHECK(validInstallId(a));
    CHECK(validInstallId(b));
    // Two ids in a row must differ; a constant would make every install look
    // like one user, and a counter would be guessable.
    CHECK(a != b);
    CHECK(a.size() == 32);

    CHECK(!validInstallId(""));
    CHECK(!validInstallId("short"));
    CHECK(!validInstallId(std::string(32, 'z')));       // not hex
    CHECK(!validInstallId(std::string(33, 'a')));       // too long
    CHECK(!validInstallId("steve@example.com"));        // hand-edited config
    CHECK(!validInstallId("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));  // uppercase
}

void testPayloadContainsOnlyTheAgreedFields() {
    TelemetryReport r;
    r.installId = newInstallId();
    r.appVersion = "0.48.0";
    r.os = osDescription();
    r.arch = archDescription();
    r.launches = 12;
    r.crashes = 1;
    r.session.seconds = 3600;
    r.session.modeSeconds["WFM"] = 3000;
    r.session.modeSeconds["RAW"] = 600;
    r.session.panels = {"map", "decoded"};
    r.session.plugins = {"ADS-B 1.0.0", "Aircraft Info 1.0.0"};
    r.session.sdrModel = sanitiseDevice("driver=uhd, product=B200, serial=EDR04ZDB2");

    const nlohmann::json j = nlohmann::json::parse(r.toJson());

    // THE FIELD INVENTORY. If someone adds a field to the payload, this fails
    // and they have to come and change the privacy notice too - which is
    // exactly the conversation that should happen.
    const std::set<std::string> allowed = {
        "id", "v", "os", "arch", "launches", "crashes",
        "sessionSec", "sdr", "modes", "panels", "plugins"};
    std::set<std::string> actual;
    for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
    CHECK(actual == allowed);

    CHECK(j["v"] == "0.48.0");
    CHECK(j["launches"] == 12);
    CHECK(j["crashes"] == 1);
    CHECK(j["modes"]["WFM"] == 3000);
    CHECK(j["panels"].size() == 2);

    // And the serial has not crept back in through the whole-document route.
    CHECK(r.toJson().find("EDR04ZDB2") == std::string::npos);
}

void testPluginNamesCannotBreakTheReport() {
    // Plugin names are third-party text. Invalid UTF-8 in one must not make
    // the report unserialisable - the same failure that took the browser
    // interface down when it was left to throw.
    TelemetryReport r;
    r.installId = newInstallId();
    r.appVersion = "0.48.0";
    r.session.plugins = {std::string("bad\xFF\xFE name 1.0.0")};
    const std::string out = r.toJson();
    CHECK(!out.empty());
    // Parses cleanly despite the rubbish going in.
    const nlohmann::json j = nlohmann::json::parse(out);
    CHECK(j["plugins"].size() == 1);
}

void testOsAndArchSayNothingIdentifying() {
    const std::string os = osDescription();
    CHECK(!os.empty());
    // A version string, not a machine name, a path or a user name.
    CHECK(os.rfind("Windows", 0) == 0 || os == "unknown");
    CHECK(os.find('@') == std::string::npos);
    CHECK(os.size() < 40);
    const std::string arch = archDescription();
    CHECK(arch == "x64" || arch == "arm64" || arch == "x86" || arch == "unknown");
}

}  // namespace

int main() {
    testDeviceSerialIsStripped();
    testInstallIdIsRandomAndValidated();
    testPayloadContainsOnlyTheAgreedFields();
    testPluginNamesCannotBreakTheReport();
    testOsAndArchSayNothingIdentifying();
    return testSummary("test_telemetry");
}
