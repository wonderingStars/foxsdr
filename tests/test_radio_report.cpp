// Tests for core/radio_report.hpp: the radio key, the text rules (held to the Worker's own parity table), the scrub, the radio lines of the log ring
// and the device-argument readers. The queue and the sender are test_radio_reporter.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/health_events.hpp"
#include "core/radio_report.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using namespace cascade::core::radioreport;

#ifndef CASCADE_SOURCE_ROOT
#error "CASCADE_SOURCE_ROOT is defined by tests/CMakeLists.txt"
#endif

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    // --- the contract's text rules, held to the Worker's parity table (557 strings) ----------------------------------------------------------
    {
        const std::string text = slurp(std::string(CASCADE_SOURCE_ROOT) + "/telemetry-worker/test-fixtures/radio-text-rules.json");
        CHECK(!text.empty());
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        CHECK(j.is_object() && j.contains("cases"));
        int n = 0;
        int bad = 0;
        if (j.is_object() && j.contains("cases")) {
            for (const nlohmann::json& c : j["cases"]) {
                const std::string want = c.value("code", std::string());
                const std::string got = textCode(c.value("text", std::string()), c.value("serial", std::string()));
                ++n;
                if (got != want) {
                    ++bad;
                    if (bad <= 8) { std::printf("PARITY %s: text=[%s] serial=[%s] want=[%s] got=[%s]\n", "radio-text-rules", c.value("text", std::string()).c_str(), c.value("serial", std::string()).c_str(), want.c_str(), got.c_str()); }
                }
            }
        }
        std::printf("text-rule parity: %d cases, %d differ\n", n, bad);
        CHECK(n >= 500);
        CHECK(bad == 0);
    }

    // --- the radio key (contract 1.1) -------------------------------------------------------------------------------------------------------
    CHECK(radioKey("uhd b200", "EDR04ZDB2") == "uhd-b200:EDR04ZDB2");
    CHECK(radioKey("RTL-SDR", "") == "rtl-sdr:-");
    CHECK(radioKey("Airspy HF+", "hf0001a2") == "airspy-hf:HF0001A2");   // the serial is upper-cased
    CHECK(modelSlug("  --Weird   Model!!  ") == "weird-model");
    CHECK(modelSlug(std::string(60, 'a')).size() == 40);
    CHECK(modelSlug("") == "");
    CHECK(radioKey("", "") == ":-");

    // --- which radio: the serial from the arguments, never an address ---------------------------------------------------------------------------
    CHECK(serialFromArgs("soapy", "driver=uhd, label=B200 EDR04ZDB2, product=B200, serial=EDR04ZDB2, type=b200") == "EDR04ZDB2");
    CHECK(serialFromArgs("rtlsdr", "serial=00000001") == "00000001");
    CHECK(serialFromArgs("rtlsdr", "index=0").empty());
    CHECK(serialFromArgs("rtltcp", "rtltcp=garage.lan:1234").empty());
    CHECK(serialFromArgs("rtltcp", "serial=1234").empty());   // an rtl_tcp server has no hardware serial the app can read
    CHECK(serialFromArgs("pluto", "uri=ip:192.168.2.1").empty());
    CHECK(serialFromArgs("soundcard", "serial=123456").empty());
    CHECK(serialFromArgs("airspyhf", "serial=HF0001A2") == "HF0001A2");
    CHECK(serialFromArgs("hackrf", "serial=a/b").empty());   // not shaped like a serial
    CHECK(serialFromArgs("hackrf", "serial=" + std::string(41, 'a')).empty());
    {
        const RadioIdentity a = identify("soapy", "driver=uhd, label=B200 EDR04ZDB2, product=B200, serial=EDR04ZDB2, type=b200");
        CHECK(a.driver == "soapy");
        CHECK(a.serial == "EDR04ZDB2");
        CHECK(a.model.find("EDR04ZDB2") == std::string::npos);
        CHECK(a.key == radioKey(a.model, a.serial));
        const RadioIdentity b = identify("rtlsdr", "serial=00000001", "RTL-SDR (serial 00000001)");
        CHECK(b.model == "RTL-SDR");
        CHECK(b.serial == "00000001");
        CHECK(b.key == "rtl-sdr:00000001");
        const RadioIdentity c = identify("rtltcp", "rtltcp=10.1.2.3:1234");
        CHECK(c.serial.empty());
        CHECK(c.model.find("10.1.2.3") == std::string::npos);
        CHECK(c.driver == "rtltcp");
        const RadioIdentity d = identify("somethingnew", "serial=XYZ12345");
        CHECK(d.driver == "other");
        const RadioIdentity e = soundCardIdentity();
        CHECK(e.driver == "soundcard" && e.model == "sound card" && e.serial.empty() && e.key == "sound-card:-");
        // the model never carries an address or a unit name, whatever the label said
        const RadioIdentity f = identify("airspy", "serial=ABCD1234", "Airspy 10.0.0.5 (serial ABCD1234)");
        CHECK(f.model.find("10.0.0.5") == std::string::npos);
        CHECK(scrubModel(f.model) == f.model);
    }

    // --- the scrub: the real D3 sentence, and every kind of thing the text rules refuse ------------------------------------------------------------
    {
        const std::string d3 = "the radio stopped answering while restoring the crystal trim: a control transfer failed (Windows error 31, serial airspyhf_sn:HF0001A2)";
        const std::string out = scrubReason(d3, "HF0001A2");
        CHECK(out.find("HF0001A2") == std::string::npos);
        CHECK(textCode(out, "HF0001A2").empty());
        CHECK(out.find("control transfer failed") != std::string::npos);   // the sentence itself survives
        CHECK(health::radioReasonWord(health::classifyRadioOpen("airspyhf", d3)) == std::string("control"));

        const std::vector<std::string> nasty = {
            "could not open C:\\Users\\alice\\AppData\\Local\\FoxSDR\\radio.cfg",
            "cannot read /home/bob/.config/foxsdr/x.json",
            "failed at \\\\NAS\\share\\iq\\file.wav now",
            "tuned to 433.920 MHz and 1.2 kHz away: failed",
            "the server 192.168.1.50:1234 refused",
            "device EDR04ZDB2 went away (bus 3, serial EDR04ZDB2)",
            "path D:/recordings/x.iq not found",
            "listening on 145.800MHz and 7,100.5 kHz",
        };
        for (const std::string& n : nasty) {
            const std::string o = scrubReason(n, "EDR04ZDB2");
            if (!textCode(o, "EDR04ZDB2").empty()) { std::printf("STILL REFUSED: [%s] -> [%s] (%s)\n", n.c_str(), o.c_str(), textCode(o, "EDR04ZDB2").c_str()); }
            CHECK(textCode(o, "EDR04ZDB2").empty());
            CHECK(o.find("alice") == std::string::npos);
            CHECK(o.find("192.168") == std::string::npos);
            CHECK(o.find("EDR04ZDB2") == std::string::npos);
            CHECK(!o.empty());
        }
        // every path becomes <path>
        CHECK(pathsToToken("opened C:\\Users\\<user>\\x\\y.log, then /home/<user>/z") == "opened <path>, then <path>");
        CHECK(pathsToToken("tcp://host:1 is not a path") == "tcp://host:1 is not a path");
        CHECK(pathsToToken("no paths here") == "no paths here");
        // empty reason is still a reason; long text is cut on a character boundary
        CHECK(!scrubReason("", "").empty());
        const std::string longText(1000, 'x');
        CHECK(utf8Length(scrubReason(longText, "")) == 400);
        const std::string wide = scrubReason(std::string(300, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9", "");
        CHECK(utf8Length(wide) <= 400);
        CHECK(clipUtf8("a\xC3\xA9" "b", 2) == "a\xC3\xA9");
    }

    // --- the radio lines of the log ring ------------------------------------------------------------------------------------------------------
    {
        std::vector<std::string> ring;
        ring.push_back("12:01:07.120 info window: opened the main window");                                             // not a radio line
        ring.push_back("12:01:07.121 warn soapy: airspyhf open failed: control transfer failed (Windows error 31)");   // a radio line
        ring.push_back("12:01:07.200 info source: tuned to 100.000 MHz");                                                // a radio line, with a frequency
        ring.push_back("12:01:07.331 warn source: the saved radio (Airspy HF+, airspyhf) did not reopen (control) - the receiver is on the signal generator");
        ring.push_back("12:01:08.000 info patch: radio node 3 would not open: C:\\Users\\alice\\x.iq");                // a radio line with a path
        ring.push_back("12:01:09.000 info bookmarks: loaded 12");                                                       // not a radio line
        ring.push_back("12:01:09.500 info vendor: serial=EDR04ZDB2 attached");
        const std::vector<std::string> got = radioLines(ring, "EDR04ZDB2");
        CHECK(got.size() == 5);
        for (const std::string& l : got) {
            CHECK(textCode(l, "EDR04ZDB2").empty());
            CHECK(l.find(" info ") == std::string::npos && l.find(" warn ") == std::string::npos);   // the ring's level word is dropped
            CHECK(l.size() <= 240);
        }
        CHECK(!got.empty() && got.front().rfind("12:01:07.121 soapy: ", 0) == 0);
        CHECK(!got.empty() && got.back().find("EDR04ZDB2") == std::string::npos);
        // at most 40, the NEWEST kept, oldest first
        std::vector<std::string> many;
        for (int i = 0; i < 100; ++i) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "12:%02d:00.000 info source: line number %d", i % 60, i);
            many.push_back(buf);
        }
        const std::vector<std::string> cut = radioLines(many, "");
        CHECK(cut.size() == 40);
        CHECK(cut.back().find("line number 99") != std::string::npos);
        CHECK(cut.front().find("line number 60") != std::string::npos);
        CHECK(radioLines({}, "").empty());
    }

    // --- helpers -----------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string a = randomHex(16);
        const std::string b = randomHex(16);
        CHECK(a.size() == 16 && b.size() == 16 && a != b);
        for (char c : a) { CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); }
        CHECK(cpuWithoutClock("Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz") == "Intel(R) Core(TM) i7-8700K CPU");
        CHECK(textCode(cpuWithoutClock("Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz"), "").empty());
        CHECK(versionText("libairspyhf 1.6.8 (x)") == "libairspyhf 1.6.8 x");
        CHECK(!kRadioReportsLiveSend);   // THE ONE SWITCH IS OFF in this build
    }

    return testSummary("test_radio_report");
}
